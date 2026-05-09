#include "VfoWidget.h"
#include "PhaseKnob.h"
#include "ComboStyle.h"
#include "GuardedSlider.h"
#include "RxApplet.h"
#include "SliceColorManager.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "core/AppSettings.h"

#include <QDateTime>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QLabel>
#include <QSlider>
#include <QGraphicsOpacityEffect>
#include <QLineEdit>
#include <QComboBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMenu>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSignalBlocker>
#include <QDir>
#include <QFile>
#include <QPixmap>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <cmath>

// QSlider that always accepts wheel events, preventing propagation to parent
// (e.g. SpectrumWidget frequency scroll) at min/max boundaries. (#547 BUG-002)
// GuardedSlider moved to GuardedSlider.h for use across all applets (#570)

// Horizontal level meter bar: maps a dBm value to a filled bar.
// Range: -130 (empty) to -20 dBm (full). Color: cyan with green tint above S9.
class LevelBar : public QWidget {
public:
    explicit LevelBar(const float& valueRef, QWidget* parent = nullptr)
        : QWidget(parent), m_value(valueRef) {}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        // Background
        p.fillRect(rect(), QColor(0x10, 0x10, 0x1c));
        // Border
        p.setPen(QColor(0x30, 0x40, 0x50));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
        // Fill: map -130...-20 dBm to 0...1
        constexpr float lo = -130.0f, hi = -20.0f;
        float frac = std::clamp((m_value - lo) / (hi - lo), 0.0f, 1.0f);
        int fillW = static_cast<int>(frac * (width() - 2));
        if (fillW > 0) {
            // Cyan below S9 (-73 dBm), green above
            QColor color = (m_value < -73.0f) ? QColor(0x00, 0xb4, 0xd8)
                                               : QColor(0x00, 0xd8, 0x60);
            p.fillRect(1, 1, fillW, height() - 2, color);
        }
    }
private:
    const float& m_value;
};

// Slider that resets to a default value on double-click.
// Extends GuardedSlider for controls-lock support (#745).
class ResetSlider : public GuardedSlider {
public:
    explicit ResetSlider(int resetVal, Qt::Orientation o, QWidget* parent = nullptr)
        : GuardedSlider(o, parent), m_resetVal(resetVal) {}
protected:
    void mouseDoubleClickEvent(QMouseEvent*) override { setValue(m_resetVal); }
private:
    int m_resetVal;
};

// ResetSlider with a small center-mark dot painted on the groove.
class CenterMarkSlider : public ResetSlider {
public:
    explicit CenterMarkSlider(int resetVal, Qt::Orientation o, QWidget* parent = nullptr)
        : ResetSlider(resetVal, o, parent) {}
protected:
    void paintEvent(QPaintEvent* ev) override {
        ResetSlider::paintEvent(ev);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        int cx = width() / 2;
        int cy = height() / 2;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#608090"));
        p.drawEllipse(QPointF(cx, cy), 2.5, 2.5);
    }
};

// Button that paints a solid left- or right-pointing triangle.
class TriBtn : public QPushButton {
public:
    enum Dir { Left, Right };
    explicit TriBtn(Dir dir, QWidget* parent = nullptr)
        : QPushButton(parent), m_dir(dir)
    {
        setFlat(false);
        setFixedSize(22, 22);
        setStyleSheet(
            "QPushButton { background: #1a2a3a; border: 1px solid #203040; "
            "border-radius: 3px; padding: 0; margin: 0; min-width: 0; min-height: 0; }"
            "QPushButton:hover { background: #203040; }"
            "QPushButton:pressed { background: #00b4d8; }");
    }
protected:
    void paintEvent(QPaintEvent* ev) override {
        QPushButton::paintEvent(ev);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xc8, 0xd8, 0xe8));
        const double cx = width() / 2.0, cy = height() / 2.0;
        if (m_dir == Left) {
            const QPointF tri[] = {{cx + 3.0, cy - 4.0}, {cx + 3.0, cy + 4.0}, {cx - 3.0, cy}};
            p.drawPolygon(tri, 3);
        } else {
            const QPointF tri[] = {{cx - 3.0, cy - 4.0}, {cx - 3.0, cy + 4.0}, {cx + 3.0, cy}};
            p.drawPolygon(tri, 3);
        }
    }
private:
    Dir m_dir;
};


namespace AetherSDR {

// ── Styles ────────────────────────────────────────────────────────────────────

// Background is painted manually in paintEvent for true alpha transparency.
static const QString kBgStyle =
    "QWidget#VfoWidgetRoot { background: transparent; border: none; }";

static const QString kFlatBtn =
    "QPushButton { background: transparent; border: none; "
    "font-size: 13px; font-weight: bold; padding: 0 6px; margin: 0; }";

static const QString kTabLblNormal =
    "QLabel { background: transparent; border: none; "
    "border-bottom: 2px solid transparent; "
    "color: #6888a0; font-size: 13px; font-weight: bold; padding: 3px 0; }";

static const QString kTabLblActive =
    "QLabel { background: transparent; border: none; "
    "border-bottom: 2px solid #00b4d8; "
    "color: #00b4d8; font-size: 13px; font-weight: bold; padding: 3px 0; }";

static const QString kDisabledBtn =
    "QPushButton:disabled { background-color: #1a1a2a; color: #556070; "
    "border: 1px solid #2a3040; }";

static const QString kDspToggle =
    "QPushButton { background: #1a2a3a; border: 1px solid #304050; border-radius: 2px; "
    "color: #c8d8e8; font-size: 13px; font-weight: bold; padding: 2px 4px; }"
    "QPushButton:checked { background: #1a6030; color: #ffffff; border: 1px solid #20a040; }"
    "QPushButton:hover { border: 1px solid #0090e0; }";

static const QString kModeBtn =
    "QPushButton { background: #1a2a3a; border: 1px solid #304050; border-radius: 2px; "
    "color: #c8d8e8; font-size: 13px; font-weight: bold; padding: 3px; }"
    "QPushButton:checked { background: #0070c0; color: #ffffff; border: 1px solid #0090e0; }"
    "QPushButton:hover { border: 1px solid #0090e0; }";

static const QString kSliderStyle =
    "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; border-radius: 2px; }"
    "QSlider::handle:horizontal { background: #c8d8e8; width: 12px; margin: -4px 0; border-radius: 6px; }"
    "QSlider::groove:vertical { background: #1a2a3a; width: 4px; border-radius: 2px; }"
    "QSlider::handle:vertical { background: #c8d8e8; height: 12px; margin: 0 -4px; border-radius: 6px; }";

static const QString kLabelStyle =
    "QLabel { background: transparent; border: none; color: #8aa8c0; font-size: 13px; }";

// ── Construction ──────────────────────────────────────────────────────────────

VfoWidget::VfoWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("VfoWidgetRoot");
    setMinimumWidth(WIDGET_W);
    setMaximumWidth(WIDGET_W);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setStyleSheet(kBgStyle);

    m_signalMeterFraction = signalDbmToMeterFraction(m_signalDbm);
    m_targetSignalMeterFraction = m_signalMeterFraction;
    m_signalMeterAnimation.setTimerType(Qt::PreciseTimer);
    m_signalMeterAnimation.setInterval(kSignalMeterAnimationIntervalMs);
    connect(&m_signalMeterAnimation, &QTimer::timeout, this, &VfoWidget::animateSignalMeter);

    buildUI();

    connect(&SliceColorManager::instance(), &SliceColorManager::colorsChanged,
            this, [this]() {
        syncFromSlice();  // refreshes badge stylesheet
        update();         // refreshes collapsed-mode painter
    });
}

void VfoWidget::wheelEvent(QWheelEvent* ev)
{
    // Filter momentum (inertial) scrolling on macOS — these arrive after the
    // physical gesture ends and would generate unwanted tune commands.
    if (ev->phase() == Qt::ScrollMomentum) { ev->accept(); return; }

    // Determine whether we should handle this event at all.
    bool shouldTune = false;
    if (m_collapsed && m_slice && !m_slice->isLocked()) {
        // In collapsed mode, scroll anywhere to tune by step size
        shouldTune = true;
    } else if (m_freqStack && m_slice && !m_slice->isLocked()) {
        // Scroll over the frequency display tunes by step size.
        QPoint local = m_freqStack->mapFrom(this, ev->position().toPoint());
        shouldTune = m_freqStack->rect().contains(local);
    }

    if (!shouldTune) { ev->accept(); return; }

    int stepHz = m_slice->stepHz();
    if (stepHz <= 0) { ev->accept(); return; }

    // Compute steps from scroll delta — same logic as SpectrumWidget.
    int steps = 0;
    if (!ev->pixelDelta().isNull()) {
        // Trackpad / high-resolution scroll: accumulate pixel delta, 1 step per ~15px.
        if (qAbs(ev->pixelDelta().x()) > qAbs(ev->pixelDelta().y())) {
            ev->ignore(); return;
        }
        m_scrollAccum += ev->pixelDelta().y();
        steps = m_scrollAccum / 15;
        m_scrollAccum -= steps * 15;
        steps = qBound(-1, steps, 1);
    } else {
        // Standard mouse wheel: angleDelta in 1/8° units, one notch = 120.
        m_angleAccum += ev->angleDelta().y();
        steps = m_angleAccum / 120;
        m_angleAccum -= steps * 120;
        steps = qBound(-1, steps, 1);
    }

    // Debounce: ignore steps arriving within 50ms of the last accepted step.
    if (steps != 0) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastWheelMs < 50) {
            steps = 0;
        } else {
            m_lastWheelMs = now;
        }
    }

    if (steps != 0) {
        double newMhz = m_slice->frequency() + steps * stepHz / 1e6;
        emit stepTuneRequested(newMhz);
    }
    ev->accept();
}

void VfoWidget::mousePressEvent(QMouseEvent* ev)
{
    ev->accept();
    if (m_collapsed) {
        // Match the painted geometry in paintEvent
        const int badgeSize = 20;
        const int margin = (width() - badgeSize) / 2;
        const QRect txRect(margin, 26, badgeSize, 16);

        // Click on painted TX badge → toggle TX assignment
        if (ev->button() == Qt::LeftButton && txRect.contains(ev->pos())) {
            if (m_slice) {
                m_slice->setTxSlice(!m_slice->isTxSlice());
            }
            update();  // repaint TX badge color
            return;
        }
        // Click anywhere else on collapsed widget → expand (deferred)
        QTimer::singleShot(0, this, [this] { setCollapsed(false); });
        return;
    }
    // Click on the slice badge → collapse the flag (deferred)
    if (ev->button() == Qt::LeftButton && m_sliceBadge && m_sliceBadge->isVisible()
        && m_sliceBadge->geometry().contains(ev->pos())) {
        QTimer::singleShot(0, this, [this] { setCollapsed(true); });
        return;
    }
    if (m_slice)
        emit sliceActivationRequested(m_slice->sliceId());
}

void VfoWidget::mouseReleaseEvent(QMouseEvent* ev)
{
    ev->accept();
}

VfoWidget::~VfoWidget()
{
    // Close/lock buttons are children of our parent (SpectrumWidget),
    // not us. During widget tree teardown, Qt may destroy them before us
    // (they're siblings, not children). QPointer auto-nulls when the
    // target is deleted, preventing double-free.
    delete m_closeSliceBtn.data();
    delete m_lockVfoBtn.data();
    delete m_recordBtn.data();
    delete m_playBtn.data();
    delete m_collapsedFreqLabel.data();
}

void VfoWidget::buildUI()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 2, 6, 0);
    root->setSpacing(2);

    // ── Header row: ANT1(rx) ANT1(tx) 3.8K  SPLIT TX ──────────────────────
    auto* hdr = new QHBoxLayout;
    hdr->setSpacing(2);
    hdr->setAlignment(Qt::AlignTop);

    m_rxAntBtn = new QPushButton("ANT1");
    m_rxAntBtn->setFlat(true);
    m_rxAntBtn->setStyleSheet(kFlatBtn + "QPushButton { color: #4488ff; }");
    connect(m_rxAntBtn, &QPushButton::clicked, this, [this] {
        if (!m_slice) return;
        QMenu menu(this);
        for (const QString& ant : m_antList) {
            auto* act = menu.addAction(ant);
            act->setCheckable(true);
            act->setChecked(ant == m_slice->rxAntenna());
        }
        if (auto* sel = menu.exec(m_rxAntBtn->mapToGlobal(QPoint(0, m_rxAntBtn->height()))))
            m_slice->setRxAntenna(sel->text());
    });
    hdr->addWidget(m_rxAntBtn);

    m_txAntBtn = new QPushButton("ANT1");
    m_txAntBtn->setFlat(true);
    m_txAntBtn->setStyleSheet(kFlatBtn + "QPushButton { color: #ff4444; }");
    connect(m_txAntBtn, &QPushButton::clicked, this, [this] {
        if (!m_slice) return;
        QMenu menu(this);
        for (const QString& ant : m_antList) {
            if (ant.startsWith("RX", Qt::CaseInsensitive))
                continue;  // skip RX-only antenna ports
            auto* act = menu.addAction(ant);
            act->setCheckable(true);
            act->setChecked(ant == m_slice->txAntenna());
        }
        if (auto* sel = menu.exec(m_txAntBtn->mapToGlobal(QPoint(0, m_txAntBtn->height()))))
            m_slice->setTxAntenna(sel->text());
    });
    hdr->addWidget(m_txAntBtn);

    m_filterWidthLbl = new QLabel("2.7K");
    m_filterWidthLbl->setStyleSheet("QLabel { background: transparent; border: none; "
                                     "color: #00c8ff; font-size: 13px; font-weight: bold; "
                                     "margin: 0; padding: 0; }");
    hdr->addWidget(m_filterWidthLbl);

    hdr->addStretch(1);

    m_splitBadge = new QPushButton("SPLIT");
    m_splitBadge->setFlat(true);
    m_splitBadge->setFixedHeight(20);  // match TX badge height
    m_splitBadge->setStyleSheet(
        "QPushButton { background: transparent; border: none; "
        "color: rgba(255,255,255,40); font-size: 11px; font-weight: bold; "
        "padding: 0px 3px; }"
        "QPushButton:hover { color: rgba(255,255,255,80); }");
    m_splitBadge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_splitBadge, &QPushButton::clicked, this, [this]() {
        if (m_splitBadge->text() == "SWAP")
            emit swapRequested();
        else
            emit splitToggled();
    });
    hdr->addWidget(m_splitBadge);

    m_txBadge = new QPushButton("TX");
    m_txBadge->setFixedSize(28, 20);
    updateTxBadgeStyle(false);
    connect(m_txBadge, &QPushButton::clicked, this, [this] {
        if (m_slice)
            m_slice->setTxSlice(!m_slice->isTxSlice());
    });
    hdr->addWidget(m_txBadge);

    m_sliceBadge = new QLabel("A");
    m_sliceBadge->setFixedSize(20, 20);
    m_sliceBadge->setAlignment(Qt::AlignCenter);
    m_sliceBadge->setCursor(Qt::PointingHandCursor);
    m_sliceBadge->setStyleSheet(
        "QLabel { background: #0070c0; color: #ffffff; "
        "border-radius: 3px; font-weight: bold; font-size: 11px; }");
    hdr->addWidget(m_sliceBadge);

    root->addLayout(hdr);


    // Close and lock buttons — children of our parent (SpectrumWidget) so they
    // can render outside our bounds. Lifecycle managed by VfoWidget destructor.
    auto* btnParent = parentWidget() ? parentWidget() : this;

    // Shared base style for the four 20x20 slice-side buttons (close, lock,
    // record, play). All use the same background circle so they line up
    // visually when stacked vertically on the slice edge.
    static const QString sliceBtnStyle =
        "QPushButton { background: rgba(255,255,255,30); border: none; "
        "border-radius: 10px; font-size: 11px; padding: 0; }"
        "QPushButton:hover { background: rgba(255,255,255,60); }";

    m_closeSliceBtn = new QPushButton("\xE2\x9C\x95", btnParent);  // ✕
    m_closeSliceBtn->setFixedSize(20, 20);
    m_closeSliceBtn->setStyleSheet(sliceBtnStyle +
        "QPushButton { color: #c8d8e8; }"
        "QPushButton:hover { background: rgba(204,32,32,180); color: #ffffff; }");
    m_closeSliceBtn->show();
    connect(m_closeSliceBtn, &QPushButton::clicked, this, [this] {
        emit closeSliceRequested();
    });

    m_lockVfoBtn = new QPushButton("\xF0\x9F\x94\x93", btnParent);  // 🔓
    m_lockVfoBtn->setFixedSize(20, 20);
    m_lockVfoBtn->setCheckable(true);
    m_lockVfoBtn->setStyleSheet(sliceBtnStyle +
        "QPushButton:checked { background: rgba(255,100,100,80); }");
    m_lockVfoBtn->show();
    connect(m_lockVfoBtn, &QPushButton::toggled, this, [this](bool locked) {
        m_lockVfoBtn->setText(locked ? "\xF0\x9F\x94\x92" : "\xF0\x9F\x94\x93");
        emit lockToggled(locked);
    });

    // Record button

    m_recordBtn = new QPushButton(QString::fromUtf8("\xe2\x8f\xba"), btnParent);  // ⏺
    m_recordBtn->setFixedSize(20, 20);
    m_recordBtn->setCheckable(true);
    m_recordBtn->setToolTip("Record slice audio");
    m_recordBtn->setStyleSheet(sliceBtnStyle +
        "QPushButton { color: #c06060; }"
        "QPushButton:checked { color: #ff2020; background: rgba(255,50,50,110); }");
    m_recordBtn->show();
    connect(m_recordBtn, &QPushButton::clicked, this, [this](bool checked) {
        emit recordToggled(checked);
    });

    // Record pulse animation
    m_recordPulse = new QTimer(this);
    m_recordPulse->setInterval(500);
    connect(m_recordPulse, &QTimer::timeout, this, [this] {
        if (!m_recordBtn) return;
        static bool dim = false;
        dim = !dim;
        m_recordBtn->setStyleSheet(
            "QPushButton { background: rgba(255,255,255,30); border: none; "
            "border-radius: 10px; font-size: 11px; padding: 0; "
            "color: " + QString(dim ? "#a03030" : "#ff3030") + "; "
            "background: rgba(255,50,50," + QString(dim ? "50" : "120") + "); }"
            "QPushButton:hover { background: rgba(255,255,255,60); }");
    });

    // Play button
    m_playBtn = new QPushButton(QString::fromUtf8("\xe2\x96\xb6"), btnParent);  // ▶
    m_playBtn->setFixedSize(20, 20);
    m_playBtn->setCheckable(true);
    m_playBtn->setEnabled(false);
    m_playBtn->setToolTip("Play recorded audio");
    m_playBtn->setStyleSheet(sliceBtnStyle +
        "QPushButton { color: #60a070; }"
        "QPushButton:checked { color: #30d050; background: rgba(50,200,80,110); }"
        "QPushButton:disabled { color: #484848; background: rgba(255,255,255,15); }");
    m_playBtn->show();
    connect(m_playBtn, &QPushButton::clicked, this, [this](bool checked) {
        emit playToggled(checked);
    });

    // ── Collapsed-mode frequency label (child of parent like close/lock) ───
    m_collapsedFreqLabel = new QLabel(btnParent);
    m_collapsedFreqLabel->setStyleSheet(
        "QLabel { background: rgba(10,10,20,220); border: 1px solid rgba(255,255,255,60);"
        " border-radius: 3px; color: #c8d8e8; font-size: 14px; font-weight: bold;"
        " padding: 1px 4px; }");
    m_collapsedFreqLabel->setAlignment(Qt::AlignCenter);
    m_collapsedFreqLabel->hide();

    // ── Frequency row (right-aligned, double-click to edit) ────────────────
    m_freqStack = new QStackedWidget;
    m_freqStack->setFixedHeight(30);

    m_freqLabel = new QLabel("14.225.000");
    m_freqLabel->setStyleSheet("QLabel { background: transparent;"
                                " border: 1px solid rgba(255,255,255,80);"
                                " border-radius: 3px;"
                                " color: #c8d8e8; font-size: 26px; font-weight: bold;"
                                " padding: 0 0 0 2px; }");
    m_freqLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_freqLabel->installEventFilter(this);
    m_freqStack->addWidget(m_freqLabel);

    m_freqEdit = new QLineEdit;
    m_freqEdit->setStyleSheet(
        "QLineEdit { background: #0a0a18; border: 1px solid #00b4d8;"
        " border-radius: 3px; color: #00e5ff; font-size: 22px;"
        " font-weight: bold; padding: 0 2px 0 0; }");
    m_freqEdit->setAlignment(Qt::AlignRight);
    // Size to fit "0000.000.000" at the label font size (4-digit MHz for XVTR/SHF)
    QFont labelFont;
    labelFont.setPixelSize(26);
    labelFont.setBold(true);
    const int stackW = QFontMetrics(labelFont).horizontalAdvance("0000.000.000") + 8;
    m_freqStack->setFixedWidth(stackW);
    m_freqEdit->setPlaceholderText("MHz (e.g. 14.225)");
    m_freqEdit->installEventFilter(this);
    m_freqStack->addWidget(m_freqEdit);
    m_freqStack->setCurrentIndex(0);  // show label by default

    connect(m_freqEdit, &QLineEdit::returnPressed, this, [this] {
        const QString text = m_freqEdit->text().trimmed();
        if (!text.isEmpty()) {
            bool ok = false;
            double freqMhz = 0.0;

            // Try parsing as MHz (e.g. "14.225", "14.225.000", "14225", "14225.0")
            QString clean = text;
            // Handle "14.225.000" format — remove dots beyond the first
            int firstDot = clean.indexOf('.');
            if (firstDot >= 0) {
                QString beforeDot = clean.left(firstDot);
                QString afterDot = clean.mid(firstDot + 1).remove('.');
                clean = beforeDot + "." + afterDot;
            }
            freqMhz = clean.toDouble(&ok);

            // If value looks like Hz or kHz (> 54 MHz is out of HF range)
            // Context-aware parsing: on XVTR bands, accept higher freqs
            const bool onXvtr = m_slice &&
                (m_slice->rxAntenna().startsWith("XVT") || m_slice->frequency() > 54.0);
            const double maxMhz = onXvtr ? 450.0 : 54.0;

            if (onXvtr) {
                // XVTR freqs always start with 3 digits (100-450 MHz).
                // Insert decimal after 3rd digit for bare integers:
                //   1446 → 144.6, 14696 → 146.96, 144600 → 144.600
                if (ok && freqMhz > 450.0 && !clean.contains('.')) {
                    // Bare integer — insert decimal after 3rd digit
                    int digits = clean.length();
                    if (digits >= 4) {
                        clean.insert(3, '.');
                        freqMhz = clean.toDouble(&ok);
                    }
                }
            } else {
                // HF: 14225 = kHz, 14225000 = Hz
                if (ok && freqMhz > 54000.0)
                    freqMhz /= 1e6;
                else if (ok && freqMhz > 54.0)
                    freqMhz /= 1e3;
            }

            if (ok && freqMhz >= 0.001 && freqMhz <= maxMhz && m_slice)
                emit directEntryCommitted(freqMhz, m_directEntrySource);
        }
        m_directEntrySource = "vfo-direct-entry";
        m_freqStack->setCurrentIndex(0);  // back to label
    });
    connect(m_freqEdit, &QLineEdit::editingFinished, this, [this] {
        m_directEntrySource = "vfo-direct-entry";
        m_freqStack->setCurrentIndex(0);
    });

    // ── Frequency row: [RADE label] [stretch] [frequency] ────────────────
    {
        auto* freqRow = new QHBoxLayout;
        freqRow->setContentsMargins(0, 0, 0, 0);
        freqRow->setSpacing(4);
#ifdef HAVE_RADE
        m_radeStatusLabel = new QLabel;
        m_radeStatusLabel->setFixedHeight(16);
        m_radeStatusLabel->setTextFormat(Qt::RichText);
        m_radeStatusLabel->setStyleSheet(
            "QLabel { color: #00b4d8; font-size: 10px; font-weight: bold;"
            " background: transparent; border: none; padding: 0; margin: 0; }");
        m_radeStatusLabel->hide();
        freqRow->addWidget(m_radeStatusLabel);
#endif
        freqRow->addStretch(1);
        freqRow->addWidget(m_freqStack);
        root->addLayout(freqRow);
    }

    // ── S-meter + dBm row (75/25 split) ────────────────────────────────────
    // S-meter bar is painted in paintEvent; spacer reserves its space.
    // dBm label sits to the right.
    auto* meterRow = new QHBoxLayout;
    meterRow->setSpacing(4);

    auto* sMeterSpacer = new QWidget;
    sMeterSpacer->setFixedHeight(22);
    sMeterSpacer->setAttribute(Qt::WA_TranslucentBackground);
    sMeterSpacer->setStyleSheet("QWidget { background: transparent; }");
    meterRow->addWidget(sMeterSpacer, 3);  // 75%

    m_dbmLabel = new QLabel("-95 dBm");
    m_dbmLabel->setStyleSheet("QLabel { background: transparent; border: none; "
                               "color: #6888a0; font-size: 11px; }");
    m_dbmLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    meterRow->addWidget(m_dbmLabel, 1);    // 25%

    root->addLayout(meterRow);

    // ── Tab bar ────────────────────────────────────────────────────────────
    m_tabBar = new QWidget;
    m_tabBar->setAttribute(Qt::WA_TranslucentBackground);
    m_tabBar->setStyleSheet("QWidget { background: transparent; }");
    auto* tabLayout = new QHBoxLayout(m_tabBar);
    tabLayout->setContentsMargins(0, 0, 0, 0);
    tabLayout->setSpacing(0);

    const QStringList tabLabels = {"\xF0\x9F\x94\x8A", "DSP", "USB", "X/RIT", "DAX"};
    for (int i = 0; i < tabLabels.size(); ++i) {
        if (i > 0) {
            auto* sep = new QLabel("|");
            sep->setStyleSheet("QLabel { background: transparent; border: none; "
                               "color: rgba(255, 255, 255, 192); font-size: 13px; padding: 0; }");
            sep->setFixedWidth(6);
            sep->setAlignment(Qt::AlignCenter);
            tabLayout->addWidget(sep);
        }
        auto* lbl = new QLabel(tabLabels[i]);
        lbl->setStyleSheet(kTabLblNormal);
        lbl->setFixedHeight(24);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setCursor(Qt::PointingHandCursor);
        lbl->installEventFilter(this);
        tabLayout->addWidget(lbl, 1);
        m_tabBtns.append(lbl);
    }
    root->addWidget(m_tabBar);

    // ── Tab content (stacked) ──────────────────────────────────────────────
    m_tabStack = new QStackedWidget;
    m_tabStack->hide();
    buildTabContent();
    root->addWidget(m_tabStack);

    // Accessible names for VoiceOver / screen reader support (#870)
    m_rxAntBtn->setAccessibleName("RX antenna");
    m_txAntBtn->setAccessibleName("TX antenna");
    m_filterWidthLbl->setAccessibleName("Filter width");
    m_splitBadge->setAccessibleName("Split mode");
    m_splitBadge->setAccessibleDescription("Toggle split transmit frequency");
    m_txBadge->setAccessibleName("TX slice selector");
    m_sliceBadge->setAccessibleName("Slice letter");
    m_freqLabel->setAccessibleName("Frequency display");
    m_freqEdit->setAccessibleName("Frequency entry");
    m_freqEdit->setAccessibleDescription("Type a frequency in MHz and press Enter");
    m_closeSliceBtn->setAccessibleName("Close slice");
    m_lockVfoBtn->setAccessibleName("VFO lock");
    m_recordBtn->setAccessibleName("Record slice audio");
    m_playBtn->setAccessibleName("Play recorded audio");
    m_dbmLabel->setAccessibleName("Signal level dBm");

    adjustSize();
}

// ── Tab content ───────────────────────────────────────────────────────────────

void VfoWidget::buildTabContent()
{
    // Tab 0: Audio
    {
        auto* m_audioTab = new QWidget;
        auto* vb = new QVBoxLayout(m_audioTab);
        vb->setContentsMargins(2, 2, 2, 2);
        vb->setSpacing(2);

        // AF row: mute-toggle label + gain slider
        auto* gainRow = new QHBoxLayout;
        gainRow->setSpacing(3);
        m_muteBtn = new QPushButton(QString::fromUtf8("AF  \xF0\x9F\x94\x8A")); // AF 🔊
        m_muteBtn->setAccessibleName("Slice audio mute");
        m_muteBtn->setCheckable(true);
        m_muteBtn->setFixedHeight(20);
        m_muteBtn->setFixedWidth(60);
        m_muteBtn->setStyleSheet(
            "QPushButton { background: #1a2a3a; border: 1px solid #304050;"
            " border-radius: 2px; color: #c8d8e8; font-size: 13px;"
            " font-weight: bold; padding: 1px 4px; }"
            "QPushButton:checked { background: #6a2020; color: #ff8080;"
            " border: 1px solid #a04040; }");
        gainRow->addWidget(m_muteBtn);
        m_afGainSlider = new GuardedSlider(Qt::Horizontal);
        m_afGainSlider->setAccessibleName("AF gain");
        m_afGainSlider->setAccessibleDescription("Audio output volume for this slice");
        m_afGainSlider->setRange(0, 100);
        m_afGainSlider->setStyleSheet(kSliderStyle);
        gainRow->addWidget(m_afGainSlider, 1);
        auto* afVal = new QLabel("0");
        afVal->setStyleSheet(kLabelStyle);
        afVal->setFixedWidth(20);
        afVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        gainRow->addWidget(afVal);
        vb->addLayout(gainRow);

        // SQL row
        auto* sqlRow = new QHBoxLayout;
        sqlRow->setSpacing(3);
        m_sqlBtn = new QPushButton("SQL");
        m_sqlBtn->setAccessibleName("Squelch");
        m_sqlBtn->setCheckable(true);
        m_sqlBtn->setFixedHeight(20);
        m_sqlBtn->setStyleSheet(kDspToggle + kDisabledBtn);
        sqlRow->addWidget(m_sqlBtn);
        m_sqlSlider = new GuardedSlider(Qt::Horizontal);
        m_sqlSlider->setAccessibleName("Squelch threshold");
        m_sqlSlider->setRange(0, 100);
        m_sqlSlider->setValue(20);
        m_sqlSlider->setStyleSheet(kSliderStyle);
        sqlRow->addWidget(m_sqlSlider, 1);
        auto* sqlVal = new QLabel("20");
        sqlVal->setStyleSheet(kLabelStyle);
        sqlVal->setFixedWidth(20);
        sqlVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sqlRow->addWidget(sqlVal);
        vb->addLayout(sqlRow);

        // AGC-T row: mode combo + threshold slider
        auto* agcRow = new QHBoxLayout;
        agcRow->setSpacing(3);
        m_agcCmb = new GuardedComboBox;
        m_agcCmb->setAccessibleName("AGC mode");
        m_agcCmb->addItems({"Off", "Slow", "Med", "Fast"});
        m_agcCmb->setFixedHeight(20);
        m_agcCmb->setFixedWidth(60);
        m_sqlBtn->setFixedWidth(60);  // match AGC combo width
        AetherSDR::applyComboStyle(m_agcCmb);
        agcRow->addWidget(m_agcCmb);
        m_agcTSlider = new GuardedSlider(Qt::Horizontal);
        m_agcTSlider->setAccessibleName("AGC threshold");
        m_agcTSlider->setRange(0, 100);
        m_agcTSlider->setValue(65);
        m_agcTSlider->setStyleSheet(kSliderStyle);
        agcRow->addWidget(m_agcTSlider, 1);
        m_agcValueLbl = new QLabel("65");
        m_agcValueLbl->setStyleSheet(kLabelStyle);
        m_agcValueLbl->setFixedWidth(20);
        m_agcValueLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        agcRow->addWidget(m_agcValueLbl);
        vb->addLayout(agcRow);

        // Pan row: DIV button + L + slider (with center marker) + R
        auto* panRow = new QHBoxLayout;
        panRow->setSpacing(3);
        m_divBtn = new QPushButton("DIV");
        m_divBtn->setAccessibleName("Diversity receive");
        m_divBtn->setCheckable(true);
        m_divBtn->setFixedHeight(20);
        m_divBtn->setFixedWidth(60);
        m_divBtn->setStyleSheet(kDspToggle);
        m_divBtn->setVisible(false);  // shown only on dual-SCU radios
        panRow->addWidget(m_divBtn);
        auto* panL = new QLabel("L");
        panL->setStyleSheet(kLabelStyle);
        panL->setFixedWidth(10);
        panL->setAlignment(Qt::AlignCenter);
        panRow->addWidget(panL);
        m_panSlider = new CenterMarkSlider(50, Qt::Horizontal);
        m_panSlider->setRange(0, 100);
        m_panSlider->setValue(50);
        m_panSlider->setStyleSheet(kSliderStyle);
        panRow->addWidget(m_panSlider, 1);
        auto* panR = new QLabel("R");
        panR->setStyleSheet(kLabelStyle);
        panR->setFixedWidth(10);
        panR->setAlignment(Qt::AlignCenter);
        panRow->addWidget(panR);
        vb->addLayout(panRow);

        // Audio tab tooltips
        m_muteBtn->setToolTip("Mutes this slice's audio output.");
        m_afGainSlider->setToolTip("Audio output volume for this slice.");
        m_sqlBtn->setToolTip("Squelch gate \u2014 silences audio when the signal drops below the threshold.");
        m_sqlSlider->setToolTip("Squelch threshold. Increase to require a stronger signal before audio opens.");
        m_agcCmb->setToolTip("AGC speed. Slow resists pumping on quiet bands; Fast tracks rapid signal changes.");
        m_agcTSlider->setToolTip(QString("AGC Threshold: %1").arg(m_agcTSlider->value()));
        m_panSlider->setToolTip("Pans audio between left and right channels.");

        // Accessible names set inline after each widget creation below (#870)

        // ESC (Enhanced Signal Clarity) panel — visible only when DIV is active
        m_escPanel = new QWidget;
        m_escPanel->setVisible(false);
        auto* escVbox = new QVBoxLayout(m_escPanel);
        escVbox->setContentsMargins(0, 0, 0, 2);
        escVbox->setSpacing(3);

        // ESC toggle + phase slider row
        auto* escTopRow = new QHBoxLayout;
        escTopRow->setSpacing(3);
        m_escBtn = new QPushButton("ESC");
        m_escBtn->setAccessibleName("Enhanced signal clarity");
        m_escBtn->setCheckable(true);
        m_escBtn->setFixedHeight(20);
        m_escBtn->setFixedWidth(60);
        m_escBtn->setStyleSheet(kDspToggle);
        escTopRow->addWidget(m_escBtn);
        auto* phaseLbl = new QLabel("P");
        phaseLbl->setStyleSheet(kLabelStyle);
        escTopRow->addWidget(phaseLbl);
        m_escPhaseSlider = new GuardedSlider(Qt::Horizontal);
        m_escPhaseSlider->setAccessibleName("ESC phase");
        m_escPhaseSlider->setRange(0, 72);   // 0–360° in 5° steps
        m_escPhaseSlider->setValue(0);
        m_escPhaseSlider->setStyleSheet(kSliderStyle);
        escTopRow->addWidget(m_escPhaseSlider, 1);
        m_escPhaseLbl = new QLabel("0\u00B0");
        m_escPhaseLbl->setStyleSheet(kLabelStyle);
        m_escPhaseLbl->setFixedWidth(28);
        m_escPhaseLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        escTopRow->addWidget(m_escPhaseLbl);
        escVbox->addLayout(escTopRow);

        // Gain vertical slider + polar plot row
        auto* escBodyRow = new QHBoxLayout;
        escBodyRow->setContentsMargins(10, 0, 30, 10);
        escBodyRow->setSpacing(4);

        // Gain vertical slider + label
        auto* gainCol = new QVBoxLayout;
        gainCol->setSpacing(1);
        m_escGainLbl = new QLabel("1.00");
        m_escGainLbl->setStyleSheet(kLabelStyle);
        m_escGainLbl->setAlignment(Qt::AlignHCenter);
        gainCol->addWidget(m_escGainLbl);
        m_escGainSlider = new GuardedSlider(Qt::Vertical);
        m_escGainSlider->setAccessibleName("ESC gain");
        m_escGainSlider->setRange(0, 200);   // 0.0 – 2.0
        m_escGainSlider->setValue(100);       // default 1.0
        m_escGainSlider->setStyleSheet(kSliderStyle);
        gainCol->addWidget(m_escGainSlider, 1);
        auto* gainLbl = new QLabel("G");
        gainLbl->setStyleSheet(kLabelStyle);
        gainLbl->setAlignment(Qt::AlignHCenter);
        gainCol->addWidget(gainLbl);
        escBodyRow->addLayout(gainCol);

        // Polar plot
        escBodyRow->addStretch();
        m_phaseKnob = new PhaseKnob;
        escBodyRow->addWidget(m_phaseKnob);
        escVbox->addLayout(escBodyRow);

        // ESC meter row: stretch + "ESC:" + level bar + dBm (right-aligned)
        auto* escMeterRow = new QHBoxLayout;
        escMeterRow->setSpacing(4);
        escMeterRow->setContentsMargins(0, 0, 10, 0);
        escMeterRow->addStretch();
        m_escMeterLbl = new QLabel("ESC:");
        m_escMeterLbl->setStyleSheet(
            "QLabel { color: #00b4d8; font-size: 11px; font-family: monospace; }");
        escMeterRow->addWidget(m_escMeterLbl);
        m_escMeterBar = new LevelBar(m_escLevelDbm);
        m_escMeterBar->setFixedHeight(8);
        m_escMeterBar->setFixedWidth(60);
        escMeterRow->addWidget(m_escMeterBar);
        m_escDbmLbl = new QLabel("--- dBm");
        m_escDbmLbl->setStyleSheet(
            "QLabel { color: #00b4d8; font-size: 11px; font-family: monospace; }");
        m_escDbmLbl->setAlignment(Qt::AlignRight);
        escMeterRow->addWidget(m_escDbmLbl);
        escVbox->addLayout(escMeterRow);

        vb->addWidget(m_escPanel);

        // ── Audio tab connects (all widgets now created) ──
        connect(m_afGainSlider, &QSlider::valueChanged, this, [this, afVal](int v) {
            afVal->setText(QString::number(v));
            if (!m_updatingFromModel) {
                if (m_slice) m_slice->setAudioGain(v);
                emit afGainChanged(v);
            }
        });
        connect(m_muteBtn, &QPushButton::toggled, this, [this](bool on) {
            if (!m_updatingFromModel && m_slice) m_slice->setAudioMute(on);
            m_muteBtn->setText(on ? QString::fromUtf8("AF  \xF0\x9F\x94\x87")    // 🔇 AF
                                  : QString::fromUtf8("AF  \xF0\x9F\x94\x8A"));  // 🔊 AF
            m_tabBtns[0]->setText(on ? QString::fromUtf8("\xF0\x9F\x94\x87")
                                     : QString::fromUtf8("\xF0\x9F\x94\x8A"));
            if (!m_updatingFromModel) emit audioMuteToggled(on);  // (#1560)
        });
        connect(m_sqlBtn, &QPushButton::toggled, this, [this](bool on) {
            if (!m_updatingFromModel && m_slice)
                m_slice->setSquelch(on, m_sqlSlider->value());
        });
        connect(m_sqlSlider, &QSlider::valueChanged, this, [this, sqlVal](int v) {
            sqlVal->setText(QString::number(v));
            if (!m_updatingFromModel && m_slice)
                m_slice->setSquelch(m_sqlBtn->isChecked(), v);
        });
        connect(m_agcCmb, &QComboBox::currentTextChanged, this, [this](const QString& text) {
            if (!m_updatingFromModel && m_slice) {
                QString mode = text.toLower();
                if (mode == "off") mode = "off";
                else if (mode == "slow") mode = "slow";
                else if (mode == "med") mode = "med";
                else if (mode == "fast") mode = "fast";
                m_slice->setAgcMode(mode);
            }
        });
        connect(m_agcTSlider, &QSlider::valueChanged, this, [this](int v) {
            if (m_agcValueLbl) m_agcValueLbl->setText(QString::number(v));
            const bool agcOff = m_slice && (m_slice->agcMode() == "off");
            m_agcTSlider->setToolTip(agcOff
                ? QString("AGC Off Level: %1").arg(v)
                : QString("AGC Threshold: %1").arg(v));
            if (!m_updatingFromModel && m_slice) {
                if (agcOff) m_slice->setAgcOffLevel(v);
                else m_slice->setAgcThreshold(v);
            }
        });
        connect(m_panSlider, &QSlider::valueChanged, this, [this](int v) {
            if (!m_updatingFromModel && m_slice) m_slice->setAudioPan(v);
            if (!m_updatingFromModel) emit rxPanChanged(v);  // (#1460)
        });
        connect(m_divBtn, &QPushButton::toggled, this, [this](bool on) {
            if (!m_updatingFromModel && m_slice)
                m_slice->setDiversity(on);
            // ESC panel only on diversity parent, not child
            m_escPanel->setVisible(on && m_slice && !m_slice->isDiversityChild());
            resize(sizeHint());
        });
        connect(m_escBtn, &QPushButton::toggled, this, [this](bool on) {
            if (!m_updatingFromModel && m_slice)
                m_slice->setEscEnabled(on);
        });
        connect(m_escPhaseSlider, &QSlider::valueChanged, this, [this](int v) {
            int deg = v * 5;  // 5° steps
            float rad = deg * static_cast<float>(M_PI) / 180.0f;
            m_escPhaseLbl->setText(QString::number(deg) + QChar(0x00B0));
            m_phaseKnob->setPhase(rad);
            if (!m_updatingFromModel && m_slice)
                m_slice->setEscPhaseShift(rad);
        });
        connect(m_escGainSlider, &QSlider::valueChanged, this, [this](int v) {
            float gain = v / 100.0f;
            m_escGainLbl->setText(QString::number(gain, 'f', 2));
            m_phaseKnob->setGain(gain);
            if (!m_updatingFromModel && m_slice)
                m_slice->setEscGain(gain);
        });

        m_tabStack->addWidget(m_audioTab);
    }

    // Tab 1: DSP — 4-column grid + RTTY Mark/Shift (mode-dependent)
    {
        auto* dspTab = new QWidget;
        auto* dspVb = new QVBoxLayout(dspTab);
        dspVb->setContentsMargins(2, 2, 2, 2);
        dspVb->setSpacing(3);

        m_dspGrid = new QGridLayout;
        m_dspGrid->setSpacing(3);

        auto makeDsp = [&](const QString& text) {
            auto* b = new QPushButton(text);
            b->setCheckable(true);
            b->setFixedHeight(26);
            b->setStyleSheet(kDspToggle);
            return b;
        };

        m_nrBtn   = makeDsp("NR");
        m_nrBtn->setAccessibleName("Noise reduction");
        m_nbBtn   = makeDsp("NB");
        m_nbBtn->setAccessibleName("Noise blanker");
        m_anfBtn  = makeDsp("ANF");
        m_anfBtn->setAccessibleName("Auto notch filter");
        m_apfBtn  = makeDsp("APF");
        m_apfBtn->setAccessibleName("CW audio peaking filter");
        m_nrlBtn  = makeDsp("NRL");
        m_nrlBtn->setAccessibleName("Leaky LMS noise reduction");
        m_nrsBtn  = makeDsp("NRS");
        m_nrsBtn->setAccessibleName("Spectral subtraction");
        m_rnnBtn  = makeDsp("RNN");
        m_rnnBtn->setAccessibleName("RNN noise reduction");
        m_nrfBtn  = makeDsp("NRF");
        m_nrfBtn->setAccessibleName("Spectral noise filter");
        m_anflBtn = makeDsp("ANFL");
        m_anflBtn->setAccessibleName("LMS notch filter");
        m_anftBtn = makeDsp("ANFT");
        m_anftBtn->setAccessibleName("FFT notch filter");
        m_apfBtn->hide();  // only visible in CW mode

        // Client-side AetherDSP launcher — same kDspToggle styling and
        // single-cell width as the radio-side toggles, but non-checkable.
        // Placed by relayoutDspGrid() at the end of the radio-side toggle list.
        m_aetherDspBtn = new QPushButton("ADSP");
        m_aetherDspBtn->setCheckable(false);
        m_aetherDspBtn->setMinimumHeight(22);
        m_aetherDspBtn->setStyleSheet(kDspToggle);
        m_aetherDspBtn->setAccessibleName("AetherDSP Settings");
        m_aetherDspBtn->setToolTip("Open AetherDSP Settings (client-side NR2 / NR4 / DFNR / RN2 / BNR / MNR)");
        connect(m_aetherDspBtn, &QPushButton::clicked, this,
                &VfoWidget::aetherDspRequested);

        // AetherVoice launcher — opens the Aetherial Audio Channel Strip.
        // 2 columns wide (cols 2-3 of the same row that hosts ADSP).
        m_aetherVoiceBtn = new QPushButton("AetherVoice");
        m_aetherVoiceBtn->setCheckable(false);
        m_aetherVoiceBtn->setMinimumHeight(22);
        m_aetherVoiceBtn->setStyleSheet(kDspToggle);
        m_aetherVoiceBtn->setAccessibleName("Aetherial Audio Channel Strip");
        m_aetherVoiceBtn->setToolTip("Open Aetherial Audio Channel Strip — unified TX DSP suite");
        connect(m_aetherVoiceBtn, &QPushButton::clicked, this,
                &VfoWidget::aetherVoiceRequested);

        // Radio-side DSP buttons only \u2014 client-side modules (NR2 / NR4 /
        // MNR / BNR / DFNR / RN2) live in the spectrum overlay menu and
        // the AetherDSP applet; users toggle them there to keep the VFO
        // grid focused on what the radio supplies.  4-column layout:
        m_dspGrid->addWidget(m_nrBtn,   0, 0);
        m_dspGrid->addWidget(m_nbBtn,   0, 1);
        m_dspGrid->addWidget(m_anfBtn,  0, 2);
        m_dspGrid->addWidget(m_apfBtn,  0, 3);
        m_dspGrid->addWidget(m_nrlBtn,  1, 0);
        m_dspGrid->addWidget(m_nrsBtn,  1, 1);
        m_dspGrid->addWidget(m_rnnBtn,  1, 2);
        m_dspGrid->addWidget(m_nrfBtn,  1, 3);
        m_dspGrid->addWidget(m_anflBtn, 2, 0);
        m_dspGrid->addWidget(m_anftBtn, 2, 1);
        dspVb->addLayout(m_dspGrid);

        // Shared DSP-level row — one slider that re-targets based on which
        // leveled DSP is most recently turned on.  Hidden when the active
        // target is None (no leveled DSP on, or only RNN / ANFT / APF on).
        {
            m_dspLevelRow = new QWidget;
            auto* lvlHb = new QHBoxLayout(m_dspLevelRow);
            lvlHb->setContentsMargins(0, 4, 0, 0);
            lvlHb->setSpacing(4);

            m_dspLevelLabel = new QLabel("NR");
            m_dspLevelLabel->setStyleSheet(
                "QLabel { color: #c8d8e8; font-size: 13px; font-weight: bold;"
                "  min-width: 40px; }");
            m_dspLevelLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            lvlHb->addWidget(m_dspLevelLabel);

            m_dspLevelSlider = new GuardedSlider(Qt::Horizontal);
            m_dspLevelSlider->setRange(0, 100);
            m_dspLevelSlider->setStyleSheet(kSliderStyle);
            lvlHb->addWidget(m_dspLevelSlider, 1);

            m_dspLevelValue = new QLabel("0");
            m_dspLevelValue->setStyleSheet(
                "QLabel { color: #c8d8e8; font-size: 10px; min-width: 24px;"
                "  padding-right: 4px; }");
            m_dspLevelValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            lvlHb->addWidget(m_dspLevelValue);

            connect(m_dspLevelSlider, &QSlider::valueChanged, this, [this](int v) {
                m_dspLevelValue->setText(QString::number(v));
                if (m_updatingFromModel || !m_slice) return;
                switch (m_dspLevelTarget) {
                    case LvlNR:   m_slice->setNrLevel(v);   break;
                    case LvlNB:   m_slice->setNbLevel(v);   break;
                    case LvlAnf:  m_slice->setAnfLevel(v);  break;
                    case LvlNrl:  m_slice->setNrlLevel(v);  break;
                    case LvlNrs:  m_slice->setNrsLevel(v);  break;
                    case LvlNrf:  m_slice->setNrfLevel(v);  break;
                    case LvlAnfl: m_slice->setAnflLevel(v); break;
                    case LvlNone: break;
                }
            });

            // Stay laid out always — toggling visibility would shift the
            // button grid up/down each time the slider's target changes.
            // Instead, keep the row in the layout and fade contents with
            // an opacity effect (0 when no DSP is targeted, 1 when one
            // is).  Stray clicks while transparent are no-ops because
            // the slider's valueChanged handler ignores LvlNone.
            auto* eff = new QGraphicsOpacityEffect(m_dspLevelRow);
            eff->setOpacity(0.0);
            m_dspLevelRow->setGraphicsEffect(eff);
            dspVb->addWidget(m_dspLevelRow);
        }

        // DSP button tooltips
        m_nrBtn->setToolTip("Radio-side noise reduction \u2014 attenuates uncorrelated background noise.");
        m_nbBtn->setToolTip("Noise blanker \u2014 detects and removes fast impulse noise from sparks and switching sources.");
        m_anfBtn->setToolTip("Auto notch filter \u2014 detects and cancels persistent unwanted tones.");
        m_apfBtn->setToolTip("CW audio peaking filter \u2014 narrows the audio passband around the CW pitch frequency to improve S/N.");
        m_nrlBtn->setToolTip("Leaky LMS adaptive filter \u2014 preserves correlated signals while removing uncorrelated noise. Best for daily SSB/CW.");
        m_nrsBtn->setToolTip("Spectral subtraction with voice activity detection \u2014 cuts noise most aggressively between words.");
        m_rnnBtn->setToolTip("Deep-learning recurrent neural network \u2014 separates speech from complex noise. Best at low SNR.");
        m_nrfBtn->setToolTip("Spectral subtraction filter \u2014 computes speech/noise probability per frequency bin to remove steady noise.");
        m_anflBtn->setToolTip("Leaky LMS notch filter \u2014 removes steady tones such as power-line hum or carriers.");
        m_anftBtn->setToolTip("FFT-based notch filter \u2014 removes up to five persistent tones from transformers or power supplies.");

        // DSP button accessible names (#870)
        // Accessible names set inline after each widget creation below (#870)

        // APF level slider (hidden unless CW mode)
        {
            m_apfContainer = new QWidget;
            auto* apfVb = new QHBoxLayout(m_apfContainer);
            apfVb->setContentsMargins(0, 2, 0, 0);
            apfVb->setSpacing(3);

            auto* lbl = new QLabel("APF");
            lbl->setStyleSheet(kLabelStyle);
            lbl->setFixedWidth(26);
            apfVb->addWidget(lbl);
            m_apfSlider = new GuardedSlider(Qt::Horizontal);
            m_apfSlider->setAccessibleName("APF bandwidth");
            m_apfSlider->setAccessibleDescription("CW audio peaking filter bandwidth");
            m_apfSlider->setRange(0, 100);
            m_apfSlider->setValue(50);
            m_apfSlider->setStyleSheet(kSliderStyle);
            m_apfSlider->setToolTip("Adjusts APF bandwidth. Higher values narrow the peak for better CW selectivity.");
            apfVb->addWidget(m_apfSlider, 1);
            m_apfValueLbl = new QLabel("50");
            m_apfValueLbl->setStyleSheet(kLabelStyle);
            m_apfValueLbl->setFixedWidth(20);
            m_apfValueLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            apfVb->addWidget(m_apfValueLbl);

            connect(m_apfSlider, &QSlider::valueChanged, this, [this](int v) {
                m_apfValueLbl->setText(QString::number(v));
                if (!m_updatingFromModel && m_slice) m_slice->setApfLevel(v);
            });

            m_apfContainer->hide();
            dspVb->addWidget(m_apfContainer);
        }

        // RTTY Mark/Shift controls (hidden unless RTTY mode)
        {
            static const QString kStepLabelStyle =
                "QLabel { font-size: 10px; background: #0a0a18; border: 1px solid #1e2e3e; "
                "border-radius: 3px; padding: 0px 2px; color: #c8d8e8; }";
            static const QString kDimLabel =
                "QLabel { background: transparent; border: none; "
                "color: #6888a0; font-size: 10px; font-weight: bold; }";

            m_rttyContainer = new QWidget;
            auto* rvb = new QVBoxLayout(m_rttyContainer);
            rvb->setContentsMargins(0, 2, 0, 0);
            rvb->setSpacing(2);

            // Row 3: labels "Mark" and "Shift" side by side, centered
            {
                auto* lblRow = new QHBoxLayout;
                lblRow->setContentsMargins(0, 0, 0, 0);
                lblRow->setSpacing(4);
                auto* markLbl = new QLabel("Mark");
                markLbl->setStyleSheet(kDimLabel);
                markLbl->setAlignment(Qt::AlignCenter);
                lblRow->addWidget(markLbl, 1);
                auto* shiftLbl = new QLabel("Space");
                shiftLbl->setStyleSheet(kDimLabel);
                shiftLbl->setAlignment(Qt::AlignCenter);
                lblRow->addWidget(shiftLbl, 1);
                rvb->addLayout(lblRow);
            }

            // Row 4: both step selectors side by side
            {
                auto* selRow = new QHBoxLayout;
                selRow->setContentsMargins(0, 0, 0, 0);
                selRow->setSpacing(4);

                static constexpr int MARK_STEP = 25;
                static constexpr int SHIFT_STEP = 5;

                // Mark selector: ◀ 2125 ▶
                auto* markMinus = new TriBtn(TriBtn::Left);
                selRow->addWidget(markMinus);
                m_markLabel = new ScrollableLabel("2125");
                m_markLabel->setAlignment(Qt::AlignCenter);
                m_markLabel->setStyleSheet(kStepLabelStyle);
                selRow->addWidget(m_markLabel, 1);
                auto* markPlus = new TriBtn(TriBtn::Right);
                selRow->addWidget(markPlus);

                auto markDown = [this] {
                    if (m_slice) m_slice->setRttyMark(m_slice->rttyMark() - MARK_STEP);
                };
                auto markUp = [this] {
                    if (m_slice) m_slice->setRttyMark(m_slice->rttyMark() + MARK_STEP);
                };
                connect(markMinus, &QPushButton::clicked, this, markDown);
                connect(markPlus, &QPushButton::clicked, this, markUp);
                connect(m_markLabel, &ScrollableLabel::scrolled, this,
                        [markUp, markDown](int dir) { if (dir > 0) markUp(); else markDown(); });

                selRow->addSpacing(4);

                // Space selector: ◀ 170 ▶
                auto* shiftMinus = new TriBtn(TriBtn::Left);
                selRow->addWidget(shiftMinus);
                m_shiftLabel = new ScrollableLabel("170");
                m_shiftLabel->setAlignment(Qt::AlignCenter);
                m_shiftLabel->setStyleSheet(kStepLabelStyle);
                selRow->addWidget(m_shiftLabel, 1);
                auto* shiftPlus = new TriBtn(TriBtn::Right);
                selRow->addWidget(shiftPlus);

                auto shiftDown = [this] {
                    if (m_slice) m_slice->setRttyShift(m_slice->rttyShift() - SHIFT_STEP);
                };
                auto shiftUp = [this] {
                    if (m_slice) m_slice->setRttyShift(m_slice->rttyShift() + SHIFT_STEP);
                };
                connect(shiftMinus, &QPushButton::clicked, this, shiftDown);
                connect(shiftPlus, &QPushButton::clicked, this, shiftUp);
                connect(m_shiftLabel, &ScrollableLabel::scrolled, this,
                        [shiftUp, shiftDown](int dir) { if (dir > 0) shiftUp(); else shiftDown(); });

                rvb->addLayout(selRow);
            }

            m_rttyContainer->hide();
            dspVb->addWidget(m_rttyContainer);
        }

        // DIG offset control (hidden unless DIGL/DIGU mode)
        // The offset centers the filter passband for widths < 3000 Hz.
        // Double-click the value to enter directly; arrows step by 10 Hz;
        // scroll wheel also steps. (fw v1.4.0.0)
        {
            static const QString kStepLabelStyle2 =
                "QLabel { font-size: 10px; background: #0a0a18; border: 1px solid #1e2e3e; "
                "border-radius: 3px; padding: 0px 2px; color: #c8d8e8; }";
            static const QString kDimLabel2 =
                "QLabel { background: transparent; border: none; "
                "color: #6888a0; font-size: 10px; font-weight: bold; }";

            m_digContainer = new QWidget;
            auto* dvb = new QVBoxLayout(m_digContainer);
            dvb->setContentsMargins(0, 2, 0, 0);
            dvb->setSpacing(2);

            auto* lbl = new QLabel("Offset");
            lbl->setStyleSheet(kDimLabel2);
            lbl->setAlignment(Qt::AlignCenter);
            dvb->addWidget(lbl);

            auto* row = new QHBoxLayout;
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(0);

            auto* minus = new TriBtn(TriBtn::Left);
            row->addWidget(minus);

            // Stacked widget: label (normal) / line edit (direct entry)
            m_digOffsetStack = new QStackedWidget;
            m_digOffsetStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

            m_digOffsetLabel = new ScrollableLabel("2210");
            m_digOffsetLabel->setAlignment(Qt::AlignCenter);
            m_digOffsetLabel->setStyleSheet(kStepLabelStyle2);
            m_digOffsetLabel->setCursor(Qt::PointingHandCursor);
            m_digOffsetLabel->setToolTip("Double-click to enter offset directly");
            m_digOffsetStack->addWidget(m_digOffsetLabel);  // index 0

            m_digOffsetEdit = new QLineEdit;
            m_digOffsetEdit->setAlignment(Qt::AlignCenter);
            m_digOffsetEdit->setStyleSheet(
                "QLineEdit { font-size: 10px; background: #0a0a18; border: 1px solid #00b4d8; "
                "border-radius: 3px; padding: 0px 2px; color: #00e5ff; }");
            m_digOffsetStack->addWidget(m_digOffsetEdit);   // index 1

            row->addWidget(m_digOffsetStack, 1);

            auto* plus = new TriBtn(TriBtn::Right);
            row->addWidget(plus);
            dvb->addLayout(row);

            // Helper to apply a validated offset value
            auto applyOffset = [this](int hz) {
                if (!m_slice) return;
                hz = qBound(0, hz, 10000);
                if (m_slice->mode() == "DIGL")
                    m_slice->setDiglOffset(hz);
                else
                    m_slice->setDiguOffset(hz);
                // Re-apply the current filter so the passband repositions
                // immediately around the new offset without requiring a preset click.
                // In wide mode (>=3000 Hz) lo/hi are anchored at ±95, so recover
                // the preset width from the anchored edge, not the span.
                int curLo = m_slice->filterLow();
                int curHi = m_slice->filterHigh();
                int width;
                if (m_slice->mode() == "DIGL")
                    width = (curHi == -95 && curLo <= -3000) ? -curLo : curHi - curLo;
                else
                    width = (curLo == 95 && curHi >= 3000) ? curHi : curHi - curLo;
                applyFilterPreset(width);
            };

            static constexpr int DIG_STEP = 10;
            connect(minus, &QPushButton::clicked, this, [this, applyOffset] {
                if (!m_slice) return;
                int cur = (m_slice->mode() == "DIGL")
                    ? m_slice->diglOffset() : m_slice->diguOffset();
                applyOffset(cur - DIG_STEP);
            });
            connect(plus, &QPushButton::clicked, this, [this, applyOffset] {
                if (!m_slice) return;
                int cur = (m_slice->mode() == "DIGL")
                    ? m_slice->diglOffset() : m_slice->diguOffset();
                applyOffset(cur + DIG_STEP);
            });
            connect(m_digOffsetLabel, &ScrollableLabel::scrolled, this,
                    [this, applyOffset](int dir) {
                if (!m_slice) return;
                int cur = (m_slice->mode() == "DIGL")
                    ? m_slice->diglOffset() : m_slice->diguOffset();
                applyOffset(cur + dir * DIG_STEP);
            });

            // Double-click label → switch to inline edit
            m_digOffsetLabel->installEventFilter(this);

            // Commit edit on Enter or focus loss.
            // Guard against double-fire: returnPressed switches stack to index 0,
            // which removes focus and triggers editingFinished a second time.
            auto commitEdit = [this, applyOffset] {
                if (m_digOffsetStack->currentIndex() != 1) return;
                m_digOffsetStack->setCurrentIndex(0);
                bool ok;
                int hz = m_digOffsetEdit->text().toInt(&ok);
                if (ok) applyOffset(hz);
            };
            connect(m_digOffsetEdit, &QLineEdit::returnPressed, this, commitEdit);
            connect(m_digOffsetEdit, &QLineEdit::editingFinished, this, commitEdit);

            m_digContainer->hide();
            dspVb->addWidget(m_digContainer);
        }

        // FM OPT controls (hidden unless FM/NFM mode)
        {
            static const QString kDirBtn =
                "QPushButton { background: #1a2a3a; border: 1px solid #304050; border-radius: 2px;"
                " color: #c8d8e8; font-size: 11px; font-weight: bold; padding: 2px 4px; }"
                "QPushButton:checked { background: #0070c0; color: #ffffff; border: 1px solid #0090e0; }"
                "QPushButton:hover { border: 1px solid #0090e0; }";
            static const QString kRevBtn =
                "QPushButton { background: #1a2a3a; border: 1px solid #304050; border-radius: 2px;"
                " color: #c8d8e8; font-size: 11px; font-weight: bold; padding: 2px 4px; }"
                "QPushButton:checked { background-color: #604000; color: #ffb800; border: 1px solid #906000; }"
                "QPushButton:hover { border: 1px solid #0090e0; }";

            m_fmContainer = new QWidget;
            auto* fvb = new QVBoxLayout(m_fmContainer);
            fvb->setContentsMargins(0, 0, 0, 0);
            fvb->setSpacing(2);

            // Tone mode + tone value on one row
            auto* toneRow = new QHBoxLayout;
            toneRow->setSpacing(2);
            m_fmToneModeCmb = new GuardedComboBox;
            m_fmToneModeCmb->addItem("Off", QString("off"));
            m_fmToneModeCmb->addItem("CTCSS TX", QString("ctcss_tx"));
            AetherSDR::applyComboStyle(m_fmToneModeCmb);
            toneRow->addWidget(m_fmToneModeCmb, 1);

            // Tone value — simplified list of common CTCSS tones
            m_fmToneValueCmb = new GuardedComboBox;
            const double tones[] = {67.0,71.9,74.4,77.0,79.7,82.5,85.4,88.5,91.5,94.8,
                97.4,100.0,103.5,107.2,110.9,114.8,118.8,123.0,127.3,131.8,
                136.5,141.3,146.2,151.4,156.7,162.2,167.9,173.8,179.9,186.2,
                192.8,203.5,206.5,210.7,218.1,225.7,229.1,233.6,241.8,250.3,254.1};
            for (double f : tones)
                m_fmToneValueCmb->addItem(QString::number(f, 'f', 1),
                                           QString::number(f, 'f', 1));
            AetherSDR::applyComboStyle(m_fmToneValueCmb);
            m_fmToneValueCmb->setEnabled(false);
            toneRow->addWidget(m_fmToneValueCmb, 1);
            fvb->addLayout(toneRow);

            connect(m_fmToneModeCmb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this](int idx) {
                if (m_fmToneModeCmb->signalsBlocked()) return;
                const QString mode = m_fmToneModeCmb->itemData(idx).toString();
                if (m_slice) m_slice->setFmToneMode(mode);
                m_fmToneValueCmb->setEnabled(mode == "ctcss_tx");
            });
            connect(m_fmToneValueCmb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this](int idx) {
                if (m_fmToneValueCmb->signalsBlocked()) return;
                if (m_slice) m_slice->setFmToneValue(m_fmToneValueCmb->itemData(idx).toString());
            });

            // Offset row
            auto* offRow = new QHBoxLayout;
            offRow->setSpacing(4);
            auto* offLbl = new QLabel("Offset:");
            offLbl->setStyleSheet(kLabelStyle);
            offRow->addWidget(offLbl);
            m_fmOffsetSpin = new QDoubleSpinBox;
            m_fmOffsetSpin->setRange(0.0, 100.0);
            m_fmOffsetSpin->setDecimals(3);
            m_fmOffsetSpin->setSingleStep(0.1);
            m_fmOffsetSpin->setSuffix(" MHz");
            m_fmOffsetSpin->setStyleSheet(
                "QDoubleSpinBox { background: #0a0a18; border: 1px solid #1e2e3e; "
                "border-radius: 3px; color: #c8d8e8; font-size: 10px; padding: 1px 2px; }"
                "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 0; }");
            offRow->addWidget(m_fmOffsetSpin, 1);
            fvb->addLayout(offRow);

            connect(m_fmOffsetSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                    this, [this](double val) {
                if (m_fmOffsetSpin->signalsBlocked()) return;
                if (!m_slice) return;
                m_slice->setFmRepeaterOffsetFreq(val);
                const QString& dir = m_slice->repeaterOffsetDir();
                if (dir == "up") m_slice->setTxOffsetFreq(val);
                else if (dir == "down") m_slice->setTxOffsetFreq(-val);
                else m_slice->setTxOffsetFreq(0);
            });

            // Direction: − | Simplex | + | REV
            auto* dirRow = new QHBoxLayout;
            dirRow->setSpacing(2);

            auto applyDir = [this](const QString& dir) {
                if (!m_slice) return;
                m_slice->setRepeaterOffsetDir(dir);
                double offset = m_slice->fmRepeaterOffsetFreq();
                if (dir == "up") m_slice->setTxOffsetFreq(offset);
                else if (dir == "down") m_slice->setTxOffsetFreq(-offset);
                else m_slice->setTxOffsetFreq(0);
                m_fmOffsetDown->setChecked(dir == "down");
                m_fmSimplexBtn->setChecked(dir == "simplex");
                m_fmOffsetUp->setChecked(dir == "up");
            };

            m_fmOffsetDown = new QPushButton(QString::fromUtf8("\xe2\x88\x92"));
            m_fmOffsetDown->setCheckable(true);
            m_fmOffsetDown->setStyleSheet(kDirBtn);
            connect(m_fmOffsetDown, &QPushButton::clicked, this, [applyDir] { applyDir("down"); });
            dirRow->addWidget(m_fmOffsetDown);

            m_fmSimplexBtn = new QPushButton("Simplex");
            m_fmSimplexBtn->setCheckable(true);
            m_fmSimplexBtn->setChecked(true);
            m_fmSimplexBtn->setStyleSheet(kDirBtn);
            connect(m_fmSimplexBtn, &QPushButton::clicked, this, [applyDir] { applyDir("simplex"); });
            dirRow->addWidget(m_fmSimplexBtn);

            m_fmOffsetUp = new QPushButton("+");
            m_fmOffsetUp->setCheckable(true);
            m_fmOffsetUp->setStyleSheet(kDirBtn);
            connect(m_fmOffsetUp, &QPushButton::clicked, this, [applyDir] { applyDir("up"); });
            dirRow->addWidget(m_fmOffsetUp);

            m_fmRevBtn = new QPushButton("REV");
            m_fmRevBtn->setCheckable(true);
            m_fmRevBtn->setStyleSheet(kRevBtn);
            connect(m_fmRevBtn, &QPushButton::toggled, this, [this](bool on) {
                if (m_fmRevBtn->signalsBlocked() || !m_slice) return;
                double offset = m_slice->fmRepeaterOffsetFreq();
                const QString& dir = m_slice->repeaterOffsetDir();
                if (dir == "up") m_slice->setTxOffsetFreq(on ? -offset : offset);
                else if (dir == "down") m_slice->setTxOffsetFreq(on ? offset : -offset);
            });
            dirRow->addWidget(m_fmRevBtn);

            fvb->addLayout(dirRow);

            m_fmContainer->hide();
            dspVb->addWidget(m_fmContainer);
        }

        // Leveled DSP buttons retarget the shared slider on each toggle.
        // ON pushes onto the activation stack (most recent at top); OFF
        // pops it and falls back to whichever earlier-activated DSP is
        // still on.  Slider hides when the stack is empty.
        auto wireLeveledDsp = [this](QPushButton* btn,
                                     void (SliceModel::* setter)(bool),
                                     DspLevelTarget tag) {
            connect(btn, &QPushButton::toggled, this, [this, setter, tag](bool on) {
                if (!m_updatingFromModel && m_slice) (m_slice->*setter)(on);
                if (on) pushDspLevelTarget(tag);
                else    popDspLevelTarget(tag);
            });
        };
        wireLeveledDsp(m_nrBtn,   &SliceModel::setNr,   LvlNR);
        wireLeveledDsp(m_nbBtn,   &SliceModel::setNb,   LvlNB);
        wireLeveledDsp(m_anfBtn,  &SliceModel::setAnf,  LvlAnf);
        wireLeveledDsp(m_nrlBtn,  &SliceModel::setNrl,  LvlNrl);
        wireLeveledDsp(m_nrsBtn,  &SliceModel::setNrs,  LvlNrs);
        wireLeveledDsp(m_nrfBtn,  &SliceModel::setNrf,  LvlNrf);
        wireLeveledDsp(m_anflBtn, &SliceModel::setAnfl, LvlAnfl);
        // Toggle-only DSPs — do not interact with the shared slider.
        connect(m_rnnBtn,  &QPushButton::toggled, this, [this](bool on) { if (!m_updatingFromModel && m_slice) m_slice->setRnn(on); });
        connect(m_anftBtn, &QPushButton::toggled, this, [this](bool on) { if (!m_updatingFromModel && m_slice) m_slice->setAnft(on); });
        connect(m_apfBtn,  &QPushButton::toggled, this, [this](bool on) { if (!m_updatingFromModel && m_slice) m_slice->setApf(on); });

        m_tabStack->addWidget(dspTab);
    }

    // Tab 2: Mode — dropdown + filter preset grid
    {
        auto* modeTab = new QWidget;
        auto* vb = new QVBoxLayout(modeTab);
        vb->setContentsMargins(2, 2, 2, 2);
        vb->setSpacing(3);

        // Mode dropdown (same style as RxApplet)
        m_modeCombo = new GuardedComboBox;
        m_modeCombo->setFixedHeight(26);
        // Default modes — replaced dynamically when slice connects and sends mode_list
        m_modeCombo->addItems({"USB", "LSB", "CW", "AM", "SAM", "FM",
                                "NFM", "DFM", "DIGU", "DIGL", "RTTY"});
#ifdef HAVE_RADE
        m_modeCombo->addItem("RADE");
#endif
        AetherSDR::applyComboStyle(m_modeCombo);
        m_modeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) {
            if (m_modeCombo->signalsBlocked()) return;
            QString mode = m_modeCombo->currentText();
#ifdef HAVE_RADE
            if (mode == "RADE") {
                emit radeActivated(true, m_slice ? m_slice->sliceId() : -1);
                return;
            }
            // Only deactivate RADE if it was active on THIS slice's widget —
            // same guard used by the quick-mode buttons below (line ~1514).
            // Without this, any slice switching away from a non-RADE mode
            // would fire radeActivated(false) and kill RADE on a different pan.
            if (m_radeActive)
                emit radeActivated(false, m_slice ? m_slice->sliceId() : -1);
#endif
            if (!m_updatingFromModel && m_slice)
                m_slice->setMode(mode);
        });

        // Top row: dropdown + 3 configurable quick-mode buttons
        // Right-click any button to reassign its mode.
        // SSB toggles USB↔LSB; DIG toggles DIGU↔DIGL.
        auto& settings = AppSettings::instance();
        m_quickModeAssign[0] = settings.value("ModeButton0", "USB").toString();
        m_quickModeAssign[1] = settings.value("ModeButton1", "CW").toString();
        m_quickModeAssign[2] = settings.value("ModeButton2", "AM").toString();

        auto* modeRow = new QHBoxLayout;
        modeRow->setSpacing(2);
        modeRow->addWidget(m_modeCombo, 1);
        for (int i = 0; i < 3; ++i) {
            auto* btn = new QPushButton(m_quickModeAssign[i]);
            btn->setCheckable(true);
            btn->setFixedHeight(26);
            btn->setStyleSheet(kModeBtn);
            m_quickModeBtns[i] = btn;

            connect(btn, &QPushButton::clicked, this, [this, i](bool checked) {
                if (!checked) {
                    // Don't let the user uncheck the active mode — re-check and bail
                    QSignalBlocker b(m_quickModeBtns[i]);
                    m_quickModeBtns[i]->setChecked(true);
                    return;
                }
                if (!m_slice) return;
                const QString& assign = m_quickModeAssign[i];
#ifdef HAVE_RADE
                if (assign == "RADE") {
                    emit radeActivated(true, m_slice->sliceId());
                    return;
                }
                // Deactivate RADE if switching away from it
                if (m_radeActive)
                    emit radeActivated(false, m_slice->sliceId());
#endif
                if (assign == "SSB") {
                    m_slice->setMode(m_slice->mode() == "USB" ? "LSB" : "USB");
                } else if (assign == "DIG") {
                    m_slice->setMode(m_slice->mode() == "DIGU" ? "DIGL" : "DIGU");
                } else {
                    m_slice->setMode(assign);
                }
            });

            // Right-click context menu to reassign
            btn->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(btn, &QPushButton::customContextMenuRequested, this, [this, i, btn](const QPoint& pos) {
                QMenu menu;
                for (const char* m : {"USB", "LSB", "SSB", "CW", "AM", "SAM",
                                      "FM", "NFM", "DFM", "RTTY", "DIGU", "DIGL", "DIG"}) {
                    menu.addAction(m, [this, i, m] {
                        m_quickModeAssign[i] = m;
                        AppSettings::instance().setValue(
                            QString("ModeButton%1").arg(i), m);
                        AppSettings::instance().save();
                        updateQuickModeButtons();
                    });
                }
#ifdef HAVE_RADE
                menu.addAction("RADE", [this, i] {
                    m_quickModeAssign[i] = "RADE";
                    AppSettings::instance().setValue(
                        QString("ModeButton%1").arg(i), "RADE");
                    AppSettings::instance().save();
                    updateQuickModeButtons();
                });
#endif
                menu.exec(btn->mapToGlobal(pos));
            });

            modeRow->addWidget(btn, 1);
        }
        vb->addLayout(modeRow);

        // Filter preset grid (4 columns, rebuilt on mode change)
        auto* filterContainer = new QWidget;
        m_filterGrid = new QGridLayout(filterContainer);
        m_filterGrid->setContentsMargins(0, 0, 0, 0);
        m_filterGrid->setSpacing(2);
        for (int c = 0; c < 4; ++c)
            m_filterGrid->setColumnStretch(c, 1);
        vb->addWidget(filterContainer);

        m_tabStack->addWidget(modeTab);
    }

    // Tab 3: X/RIT — toggle, zero, ◀, Hz label, ▶ (matching RxApplet)
    {
        static constexpr int RIT_STEP_HZ = 10;

        static const QString kAmberActive =
            "QPushButton:checked { background-color: #604000; color: #ffb800; "
            "border: 1px solid #906000; }";
        static const QString kRitBtnStyle =
            "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
            "border-radius: 2px; color: #c8d8e8; font-size: 11px; font-weight: bold; "
            "padding: 1px 4px; }" + kAmberActive;
        static const QString kZeroBtnStyle =
            "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
            "border-radius: 2px; color: #c8d8e8; font-size: 11px; font-weight: bold; "
            "padding: 1px 4px; }"
            "QPushButton:hover { background: #203040; }";
        static const QString kHzLabelStyle =
            "QLabel { font-size: 10px; background: #0a0a18; border: 1px solid #1e2e3e; "
            "border-radius: 3px; padding: 0px 2px; color: #c8d8e8; }";

        auto* ritTab = new QWidget;
        auto* vb = new QVBoxLayout(ritTab);
        vb->setContentsMargins(2, 2, 2, 2);
        vb->setSpacing(2);

        // RIT row: toggle | 0 | ◀ | +0 Hz | ▶
        {
            auto* row = new QHBoxLayout;
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(0);

            m_ritBtn = new QPushButton("RIT");
            m_ritBtn->setCheckable(true);
            m_ritBtn->setFixedHeight(22);
            m_ritBtn->setStyleSheet(kRitBtnStyle);
            m_ritBtn->setToolTip("Receive Incremental Tuning \u2014 offsets the receive frequency without moving transmit.");
            row->addWidget(m_ritBtn);

            auto* zero = new QPushButton("0");
            zero->setFixedHeight(22);
            zero->setStyleSheet(kZeroBtnStyle);
            connect(zero, &QPushButton::clicked, this, [this] {
                if (m_slice) m_slice->setRit(m_ritBtn->isChecked(), 0);
            });
            row->addSpacing(2);
            row->addWidget(zero);
            row->addSpacing(2);

            auto* minus = new TriBtn(TriBtn::Left);
            row->addWidget(minus);

            m_ritLabel = new ScrollableLabel("+0 Hz");
            m_ritLabel->setAlignment(Qt::AlignCenter);
            m_ritLabel->setStyleSheet(kHzLabelStyle);
            row->addWidget(m_ritLabel, 1);

            auto* plus = new TriBtn(TriBtn::Right);
            row->addWidget(plus);

            connect(m_ritBtn, &QPushButton::toggled, this, [this](bool on) {
                if (!m_updatingFromModel && m_slice) m_slice->setRit(on, m_slice->ritFreq());
            });
            connect(minus, &QPushButton::clicked, this, [this] {
                if (m_slice) m_slice->setRit(m_ritBtn->isChecked(), m_slice->ritFreq() - RIT_STEP_HZ);
            });
            connect(plus, &QPushButton::clicked, this, [this] {
                if (m_slice) m_slice->setRit(m_ritBtn->isChecked(), m_slice->ritFreq() + RIT_STEP_HZ);
            });
            connect(m_ritLabel, &ScrollableLabel::scrolled, this, [this](int dir) {
                if (m_slice) m_slice->setRit(m_ritBtn->isChecked(),
                    m_slice->ritFreq() + dir * RIT_STEP_HZ);
            });

            vb->addLayout(row);
        }

        // XIT row: toggle | 0 | ◀ | +0 Hz | ▶
        {
            auto* row = new QHBoxLayout;
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(0);

            m_xitBtn = new QPushButton("XIT");
            m_xitBtn->setCheckable(true);
            m_xitBtn->setFixedHeight(22);
            m_xitBtn->setStyleSheet(kRitBtnStyle);
            m_xitBtn->setToolTip("Transmit Incremental Tuning \u2014 offsets the transmit frequency without moving receive.");
            row->addWidget(m_xitBtn);

            auto* zero = new QPushButton("0");
            zero->setFixedHeight(22);
            zero->setStyleSheet(kZeroBtnStyle);
            connect(zero, &QPushButton::clicked, this, [this] {
                if (m_slice) m_slice->setXit(m_xitBtn->isChecked(), 0);
            });
            row->addSpacing(2);
            row->addWidget(zero);
            row->addSpacing(2);

            auto* minus = new TriBtn(TriBtn::Left);
            row->addWidget(minus);

            m_xitLabel = new ScrollableLabel("+0 Hz");
            m_xitLabel->setAlignment(Qt::AlignCenter);
            m_xitLabel->setStyleSheet(kHzLabelStyle);
            row->addWidget(m_xitLabel, 1);

            auto* plus = new TriBtn(TriBtn::Right);
            row->addWidget(plus);

            connect(m_xitBtn, &QPushButton::toggled, this, [this](bool on) {
                if (!m_updatingFromModel && m_slice) m_slice->setXit(on, m_slice->xitFreq());
            });
            connect(minus, &QPushButton::clicked, this, [this] {
                if (m_slice) m_slice->setXit(m_xitBtn->isChecked(), m_slice->xitFreq() - RIT_STEP_HZ);
            });
            connect(plus, &QPushButton::clicked, this, [this] {
                if (m_slice) m_slice->setXit(m_xitBtn->isChecked(), m_slice->xitFreq() + RIT_STEP_HZ);
            });
            connect(m_xitLabel, &ScrollableLabel::scrolled, this, [this](int dir) {
                if (m_slice) m_slice->setXit(m_xitBtn->isChecked(),
                    m_slice->xitFreq() + dir * RIT_STEP_HZ);
            });

            vb->addLayout(row);
        }

        m_tabStack->addWidget(ritTab);
    }

    // Tab 4: DAX
    {
        auto* daxTab = new QWidget;
        auto* vb = new QVBoxLayout(daxTab);
        vb->setContentsMargins(2, 2, 2, 2);
        vb->setSpacing(2);

        auto* row = new QHBoxLayout;
        row->setSpacing(3);
        auto* lbl = new QLabel("DAX Ch");
        lbl->setStyleSheet(kLabelStyle);
        row->addWidget(lbl);
        m_daxCmb = new GuardedComboBox;
        m_daxCmb->addItems({"Off", "1", "2", "3", "4"});
        AetherSDR::applyComboStyle(m_daxCmb);
        row->addWidget(m_daxCmb, 1);
        vb->addLayout(row);

        connect(m_daxCmb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) {
            if (!m_updatingFromModel && m_slice)
                m_slice->setDaxChannel(idx);  // 0=Off, 1-4=channels
        });

        m_tabStack->addWidget(daxTab);
    }
}

// ── Tab switching ─────────────────────────────────────────────────────────────

void VfoWidget::showTab(int index)
{
    if (m_activeTab == index) {
        // Toggle off — collapse content
        m_tabStack->hide();
        m_tabBtns[m_activeTab]->setStyleSheet(kTabLblNormal);
        m_activeTab = -1;
    } else {
        if (m_activeTab >= 0)
            m_tabBtns[m_activeTab]->setStyleSheet(kTabLblNormal);
        m_activeTab = index;
        m_tabBtns[index]->setStyleSheet(kTabLblActive);
        m_tabStack->setCurrentIndex(index);
        m_tabStack->show();
    }
    adjustSize();
}

// ── Collapsed flag toggle ─────────────────────────────────────────────────────

void VfoWidget::setCollapsed(bool collapsed)
{
    if (m_collapsed == collapsed) return;
    m_collapsed = collapsed;

    // Persist per-slice preference
    if (m_slice) {
        auto& s = AppSettings::instance();
        s.setValue(QString("SliceFlagCollapsed_%1").arg(m_slice->sliceId()),
                   collapsed ? "True" : "False");
    }

    if (collapsed) {
        // Remember which children were already hidden so we don't show them on expand
        m_hiddenBeforeCollapse.clear();
        const QList<QWidget*> children = findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        for (QWidget* child : children) {
            if (!child->isVisible()) {
                m_hiddenBeforeCollapse.insert(child);
            }
            child->setVisible(false);
            // Make children ignore mouse events so external code that shows
            // them (e.g. updateSplitBadge) can't intercept our clicks
            child->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        }
        // Hide external buttons (and remember their state)
        if (m_closeSliceBtn) {
            if (!m_closeSliceBtn->isVisible()) m_hiddenBeforeCollapse.insert(m_closeSliceBtn);
            m_closeSliceBtn->hide();
        }
        if (m_lockVfoBtn) {
            if (!m_lockVfoBtn->isVisible()) m_hiddenBeforeCollapse.insert(m_lockVfoBtn);
            m_lockVfoBtn->hide();
        }
        if (m_recordBtn) {
            if (!m_recordBtn->isVisible()) m_hiddenBeforeCollapse.insert(m_recordBtn);
            m_recordBtn->hide();
        }
        if (m_playBtn) {
            if (!m_playBtn->isVisible()) m_hiddenBeforeCollapse.insert(m_playBtn);
            m_playBtn->hide();
        }

        // Resize to narrow collapsed width with fixed height for painted content
        setMinimumWidth(COLLAPSED_W);
        setMaximumWidth(COLLAPSED_W);
        setFixedHeight(44);  // slice badge (20) + gap (2) + TX badge (16) + padding (6)

        // Show collapsed frequency label and position it immediately
        if (m_collapsedFreqLabel) {
            updateFreqLabel();
            m_collapsedFreqLabel->setText(m_freqLabel->text());
            m_collapsedFreqLabel->adjustSize();
            m_collapsedFreqLabel->show();

            // Position now based on current widget location
            const int freqGap = 2;
            int freqH = m_collapsedFreqLabel->sizeHint().height();
            int freqW = m_collapsedFreqLabel->sizeHint().width();
            int freqY = pos().y() + (44 - freqH) / 2;
            int freqX = m_lastOnLeft
                ? pos().x() - freqW - freqGap
                : pos().x() + COLLAPSED_W + freqGap;
            m_collapsedFreqLabel->move(freqX, freqY);
        }
    } else {
        // Restore full width, remove fixed height constraint
        setMinimumWidth(WIDGET_W);
        setMaximumWidth(WIDGET_W);
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);

        // Hide collapsed frequency label
        if (m_collapsedFreqLabel) {
            m_collapsedFreqLabel->hide();
        }

        // Restore only widgets that were visible before collapse.
        // Block signals on interactive buttons during restore to prevent
        // spurious clicked/toggled signals from visibility changes.
        const QList<QWidget*> children = findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        for (QWidget* child : children) {
            child->setAttribute(Qt::WA_TransparentForMouseEvents, false);
            if (!m_hiddenBeforeCollapse.contains(child)) {
                QSignalBlocker sb(child);
                child->setVisible(true);
            }
        }
        // Tab stack starts hidden (opened by tab clicks)
        if (m_tabStack) {
            m_tabStack->hide();
            m_activeTab = -1;
            for (QLabel* lbl : m_tabBtns) {
                lbl->setStyleSheet(kTabLblNormal);
            }
        }
        // Restore external buttons to pre-collapse state and reposition them
        // based on the new expanded width (they were positioned for COLLAPSED_W)
        {
            const int btnSize = 20;
            const int gap = 2;
            int btnX;
            if (m_lastOnLeft)
                btnX = pos().x() - btnSize - gap;
            else
                btnX = pos().x() + WIDGET_W + gap;

            int btnY = pos().y();
            if (m_closeSliceBtn && !m_hiddenBeforeCollapse.contains(m_closeSliceBtn)) {
                m_closeSliceBtn->show();
                m_closeSliceBtn->move(btnX, btnY);
                btnY += btnSize + gap;
            }
            if (m_lockVfoBtn && !m_hiddenBeforeCollapse.contains(m_lockVfoBtn)) {
                m_lockVfoBtn->show();
                m_lockVfoBtn->move(btnX, btnY);
                btnY += btnSize + gap;
            }
            if (m_recordBtn && !m_hiddenBeforeCollapse.contains(m_recordBtn)) {
                m_recordBtn->show();
                m_recordBtn->move(btnX, btnY);
                btnY += btnSize + gap;
            }
            if (m_playBtn && !m_hiddenBeforeCollapse.contains(m_playBtn)) {
                m_playBtn->show();
                m_playBtn->move(btnX, btnY);
            }
        }
        m_hiddenBeforeCollapse.clear();

        // Let syncFromSlice restore mode-dependent widget visibility
        syncFromSlice();
    }

    adjustSize();
    update();

    // Trigger parent repaint so SpectrumWidget repositions us and the freq label
    if (parentWidget()) {
        parentWidget()->update();
    }
}

// ── Positioning ───────────────────────────────────────────────────────────────

void VfoWidget::setDiversityAllowed(bool allowed)
{
    if (m_divBtn) m_divBtn->setVisible(allowed);
    // ESC panel only visible when DIV is active on a dual-SCU radio
    if (m_escPanel && !allowed) {
        m_escPanel->setVisible(false);
        resize(sizeHint());
    }
}

void VfoWidget::setSmartSdrPlus(bool has)
{
    if (m_hasSmartSdrPlus == has) return;
    m_hasSmartSdrPlus = has;
    if (m_slice) rebuildFilterButtons();
}

void VfoWidget::setHasExtendedDsp(bool has)
{
    m_hasExtendedDsp = has;
}

// ── Per-slice VFO marker display prefs (#1526) ───────────────────────────────

void VfoWidget::setMarkerWidth(int widthPx)
{
    // Snap to one of the supported states: 0 (off), 1, 3.
    if (widthPx <= 0)      widthPx = 0;
    else if (widthPx <= 1) widthPx = 1;
    else                   widthPx = 3;

    if (m_markerWidth != widthPx) {
        m_markerWidth = widthPx;
        saveDisplayPrefs();
        emit markerStyleChanged(m_markerWidth, m_filterEdgesHidden);
    }
    if (m_markerThicknessBtn) {
        const QString label = (widthPx == 0) ? QStringLiteral("Marker: Off")
                            : QStringLiteral("Marker: %1px").arg(widthPx);
        m_markerThicknessBtn->setText(label);
    }
}

void VfoWidget::setFilterEdgesHidden(bool hide)
{
    if (m_filterEdgesHidden != hide) {
        m_filterEdgesHidden = hide;
        saveDisplayPrefs();
        emit markerStyleChanged(m_markerWidth, m_filterEdgesHidden);
    }
    if (m_edgesBtn) m_edgesBtn->setChecked(!hide);
}

void VfoWidget::loadDisplayPrefs()
{
    if (!m_slice) return;
    auto& s = AppSettings::instance();
    const QString keyW = QStringLiteral("Slice%1_MarkerWidth").arg(m_slice->sliceId());
    const QString keyT = QStringLiteral("Slice%1_MarkerThin").arg(m_slice->sliceId());
    const QString keyH = QStringLiteral("Slice%1_FilterEdgesHidden").arg(m_slice->sliceId());
    if (s.contains(keyW)) {
        m_markerWidth = s.value(keyW, "1").toString().toInt();
    } else {
        // Migrate from the old MarkerThin bool: True (thin) → 1, False (thick) → 3.
        m_markerWidth = (s.value(keyT, "False").toString() == "True") ? 1 : 3;
        if (s.contains(keyT))
            s.remove(keyT);
    }
    if (m_markerWidth != 0 && m_markerWidth != 1 && m_markerWidth != 3)
        m_markerWidth = 1;
    m_filterEdgesHidden = s.value(keyH, "False").toString() == "True";
}

void VfoWidget::saveDisplayPrefs()
{
    if (!m_slice) return;
    auto& s = AppSettings::instance();
    const QString keyW = QStringLiteral("Slice%1_MarkerWidth").arg(m_slice->sliceId());
    const QString keyH = QStringLiteral("Slice%1_FilterEdgesHidden").arg(m_slice->sliceId());
    s.setValue(keyW, QString::number(m_markerWidth));
    s.setValue(keyH, m_filterEdgesHidden ? "True" : "False");
    s.save();
}

void VfoWidget::setEscLevel(float dbm)
{
    m_escLevelDbm = dbm;
    if (m_escDbmLbl)
        m_escDbmLbl->setText(QString("%1 dBm").arg(dbm, 0, 'f', 0));
    if (m_escMeterBar)
        m_escMeterBar->update();
}

void VfoWidget::setAfGain(int pct)
{
    if (m_afGainSlider) {
        m_updatingFromModel = true;
        m_afGainSlider->setValue(pct);
        m_updatingFromModel = false;
    }
}

void VfoWidget::updatePosition(int vfoX, int specTop, FlagDir dir)
{
    const int w = width();
    bool onLeft = true;

    if (dir == ForceLeft) {
        onLeft = true;
    } else if (dir == ForceRight) {
        onLeft = false;
    } else {
        // Auto: use mode-based default
        bool lowerSideband = false;
        if (m_slice) {
            const QString mode = m_slice->mode();
            lowerSideband = (mode == "LSB" || mode == "DIGL" || mode == "CWL");
        }
        onLeft = !lowerSideband;
    }

    // 20px dead-band: only flip side when the widget clearly overruns the edge.
    // Without this, the flip threshold can oscillate frame-to-frame while
    // m_centerMhz is animating, snapping the VFO panel back and forth.
    constexpr int kEdgeHysteresis = 20;

    int x;
    if (onLeft) {
        x = vfoX - w;
        // Flip to right only when clearly clipping the left edge
        if (x < -kEdgeHysteresis) {
            x = vfoX;
            onLeft = false;
        }
    } else {
        x = vfoX;
        // Flip to left only when clearly clipping the right edge
        const int parentW = parentWidget() ? parentWidget()->width() : INT_MAX;
        if (x + w > parentW + kEdgeHysteresis) {
            x = vfoX - w;
            onLeft = true;
        }
    }

    // Skip all moves if position unchanged — prevents repaint cascade on QRhiWidget
    const QPoint newPos(x, specTop);
    if (pos() == newPos && m_lastOnLeft == onLeft)
        return;
    m_lastOnLeft = onLeft;

    move(newPos);

    // Position close/lock/record/play buttons stacked vertically on the side opposite the marker
    if (m_closeSliceBtn && m_lockVfoBtn) {
        const int btnSize = 20;
        const int gap = 2;
        int btnX;
        if (onLeft)
            btnX = x - btnSize - gap;  // left of VFO widget
        else
            btnX = x + w + gap;        // right of VFO widget

        int btnY = specTop;
        if (!m_collapsed) {
            m_closeSliceBtn->move(btnX, btnY);
            btnY += btnSize + gap;

            m_lockVfoBtn->move(btnX, btnY);
            btnY += btnSize + gap;

            if (m_recordBtn) {
                m_recordBtn->move(btnX, btnY);
                btnY += btnSize + gap;
            }
            if (m_playBtn) {
                m_playBtn->move(btnX, btnY);
            }
        }
    }

    // Position collapsed frequency label next to the collapsed widget
    if (m_collapsedFreqLabel && m_collapsed) {
        const int freqGap = 2;
        int freqX;
        int freqH = m_collapsedFreqLabel->sizeHint().height();
        int freqY = specTop + (height() - freqH) / 2;  // vertically centered
        int freqW = m_collapsedFreqLabel->sizeHint().width();
        if (onLeft) {
            freqX = x - freqW - freqGap;
        } else {
            freqX = x + w + freqGap;
        }
        m_collapsedFreqLabel->move(freqX, freqY);
    }
}

// ── S-Meter bar (custom paint) ────────────────────────────────────────────────

void VfoWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Opaque dark background so passband shading doesn't bleed through
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x0a, 0x0a, 0x14));
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 3, 3);

    // Subtle white overlay for depth
    p.setPen(QColor(255, 255, 255, 13));
    p.setBrush(QColor(255, 255, 255, 13));
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 3, 3);

    if (m_collapsed) {
        // ── Collapsed mode: draw slice badge and TX badge via painter ──────
        const int badgeSize = 20;
        const int margin = (width() - badgeSize) / 2;
        int yPos = 4;

        // Slice letter badge
        int sliceId = m_slice ? m_slice->sliceId() : 0;
        QRect badgeRect(margin, yPos, badgeSize, badgeSize);
        p.setBrush(SliceColorManager::instance().activeColor(sliceId));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(badgeRect, 3, 3);

        QFont badgeFont = p.font();
        badgeFont.setPixelSize(11);
        badgeFont.setBold(true);
        p.setFont(badgeFont);
        p.setPen(QColor(0, 0, 0));
        const char letters[] = "ABCDEFGH";
        QChar letter = (sliceId >= 0 && sliceId < 8) ? QChar(letters[sliceId]) : QChar('?');
        p.drawText(badgeRect, Qt::AlignCenter, QString(letter));

        yPos += badgeSize + 2;

        // TX badge
        bool isTx = m_slice && m_slice->isTxSlice();
        QRect txRect(margin, yPos, badgeSize, 16);
        p.setPen(Qt::NoPen);
        p.setBrush(isTx ? QColor(0xcc, 0x00, 0x00) : QColor(0x40, 0x40, 0x40));
        p.drawRoundedRect(txRect, 2, 2);

        QFont txFont = p.font();
        txFont.setPixelSize(10);
        txFont.setBold(true);
        p.setFont(txFont);
        p.setPen(isTx ? QColor(0xff, 0xff, 0xff) : QColor(0x80, 0x80, 0x80));
        p.drawText(txRect, Qt::AlignCenter, "TX");

        return;  // skip S-meter painting
    }

    p.setRenderHint(QPainter::Antialiasing, false);

    // Bar rect: drawn in the S-meter row (75% left portion)
    const int barX = 6;
    const int barW = (width() - 12) * 3 / 4;  // 75% of widget width
    const int barY = m_dbmLabel->y() + (m_dbmLabel->height() - 6) / 2;
    const int barH = 6;

    // Background
    p.fillRect(barX, barY, barW, barH, QColor(0x10, 0x18, 0x20));

    // S-meter scale: S0=-127, S9=-73 (6 dB per S-unit), S9+60=-13
    // S0–S9 occupies left 60%, S9–S9+60 occupies right 40%
    const int s9X = barX + barW * 60 / 100;  // S9 boundary pixel

    // Signal fill — color gradient matching SmartSDR visual convention
    const int fillW = static_cast<int>(m_signalMeterFraction * barW);

    if (fillW > 0) {
        QLinearGradient grad(barX, 0, barX + barW, 0);
        grad.setColorAt(0.00, QColor(0x00, 0x90, 0x30));  // dark green  — S0
        grad.setColorAt(0.30, QColor(0x00, 0xc0, 0x40));  // green       — ~S5
        grad.setColorAt(0.50, QColor(0xd4, 0xc0, 0x00));  // yellow      — ~S7
        grad.setColorAt(0.70, QColor(0xdd, 0x14, 0x00));  // red         — S9+10
        grad.setColorAt(0.85, QColor(0xff, 0x00, 0x00));  // bright red  — S9+30
        grad.setColorAt(1.00, QColor(0xff, 0x00, 0x00));  // bright red  — S9+60
        p.fillRect(barX, barY, fillW, barH, grad);
    }

    // ── Scale bar with tick marks below the S-meter ────────────────────────
    const int scaleY = barY + barH + 2;
    const int tickH  = 3;

    // Horizontal line: blue from start to S9, red from S9 to end
    p.setPen(QColor(0x30, 0x80, 0xff));
    p.drawLine(barX, scaleY, s9X, scaleY);
    p.setPen(QColor(0xd0, 0x20, 0x20));
    p.drawLine(s9X, scaleY, barX + barW, scaleY);

    p.setPen(QColor(0x30, 0x80, 0xff));  // blue for S1–S9

    // S-unit ticks: S1, S3, S5, S7, S9 — S1 at left edge, S9 at 60%
    for (int s = 1; s <= 9; s += 2) {
        float sf = static_cast<float>(s - 1) / 8.0f * 0.6f;
        int tx = barX + static_cast<int>(sf * barW);
        int h = (s == 9) ? tickH + 1 : tickH;
        p.drawLine(tx, scaleY, tx, scaleY + h);
    }

    p.setPen(QColor(0xd0, 0x20, 0x20));  // red for +20, +40

    // +20 tick
    {
        float sf = 0.6f + (20.0f / 60.0f) * 0.4f;
        int tx = barX + static_cast<int>(sf * barW);
        p.drawLine(tx, scaleY, tx, scaleY + tickH);
    }
    // +40 tick
    {
        float sf = 0.6f + (40.0f / 60.0f) * 0.4f;
        int tx = barX + static_cast<int>(sf * barW);
        p.drawLine(tx, scaleY, tx, scaleY + tickH);
    }

    // Scale labels
    QFont scaleFont = p.font();
    scaleFont.setPixelSize(7);
    scaleFont.setBold(true);
    p.setFont(scaleFont);

    const int lblY = scaleY + tickH + 7;

    // Blue labels: 1, 3, 5, 7, 9 — S1 at left edge, S9 at 60%
    p.setPen(QColor(0x30, 0x80, 0xff));
    for (int s : {1, 3, 5, 7, 9}) {
        float sf = static_cast<float>(s - 1) / 8.0f * 0.6f;
        int tx = barX + static_cast<int>(sf * barW);
        p.drawText(tx - 3, lblY, QString::number(s));
    }

    // Red labels: +20, +40
    p.setPen(QColor(0xd0, 0x20, 0x20));
    {
        float sf = 0.6f + (20.0f / 60.0f) * 0.4f;
        int tx = barX + static_cast<int>(sf * barW);
        p.drawText(tx - 6, lblY, "+20");
    }
    {
        float sf = 0.6f + (40.0f / 60.0f) * 0.4f;
        int tx = barX + static_cast<int>(sf * barW);
        p.drawText(tx - 6, lblY, "+40");
    }

}

// ── Signal level ──────────────────────────────────────────────────────────────

void VfoWidget::setSignalLevel(float dbm)
{
    m_signalDbm = dbm;
    m_dbmLabel->setText(QString("%1 dBm").arg(static_cast<int>(dbm)));
    updateSignalMeterTarget();
}

float VfoWidget::signalDbmToMeterFraction(float dbm)
{
    constexpr float kS0Dbm = -127.0f;
    constexpr float kS9Dbm = -73.0f;
    constexpr float kS9Plus60Dbm = -13.0f;

    if (dbm <= kS0Dbm) {
        return 0.0f;
    }
    if (dbm <= kS9Dbm) {
        return (dbm - kS0Dbm) / (kS9Dbm - kS0Dbm) * 0.6f;
    }
    if (dbm <= kS9Plus60Dbm) {
        return 0.6f + (dbm - kS9Dbm) / (kS9Plus60Dbm - kS9Dbm) * 0.4f;
    }
    return 1.0f;
}

void VfoWidget::updateSignalMeterTarget()
{
    m_targetSignalMeterFraction = signalDbmToMeterFraction(m_signalDbm);

    if (qAbs(m_targetSignalMeterFraction - m_signalMeterFraction) <= kSignalMeterSnapEpsilon) {
        m_signalMeterFraction = m_targetSignalMeterFraction;
        if (m_signalMeterAnimation.isActive()) {
            m_signalMeterAnimation.stop();
        }
        update();
        return;
    }

    if (!m_signalMeterAnimation.isActive()) {
        m_signalMeterElapsed.restart();
        m_signalMeterAnimation.start();
    }
}

void VfoWidget::animateSignalMeter()
{
    const qint64 elapsedMs = m_signalMeterElapsed.restart();
    if (elapsedMs <= 0) {
        return;
    }

    const float delta = m_targetSignalMeterFraction - m_signalMeterFraction;
    const float elapsedSeconds = static_cast<float>(elapsedMs) / 1000.0f;
    const float timeConstant = (delta >= 0.0f) ? kSignalMeterAttackTimeSeconds
                                               : kSignalMeterReleaseTimeSeconds;
    const float alpha = 1.0f - std::exp(-elapsedSeconds / timeConstant);

    if (qAbs(delta) <= kSignalMeterSnapEpsilon) {
        m_signalMeterFraction = m_targetSignalMeterFraction;
        m_signalMeterAnimation.stop();
    } else {
        m_signalMeterFraction += delta * alpha;
    }

    update();
}

// ── Slice connection ──────────────────────────────────────────────────────────

void VfoWidget::setSlice(SliceModel* slice)
{
    qDebug() << "VfoWidget::setSlice:" << (slice ? slice->sliceId() : -1)
             << "old:" << (m_slice ? m_slice->sliceId() : -1);
    if (m_slice)
        m_slice->disconnect(this);
    m_slice = slice;
    if (!m_slice) return;

    // Load per-slice display prefs now that we know the slice ID, then push
    // them out so the SpectrumWidget's overlay picks up the saved style. This
    // runs after wireVfoWidget() has connected markerStyleChanged (#1526).
    loadDisplayPrefs();
    emit markerStyleChanged(m_markerWidth, m_filterEdgesHidden);

    // Frequency
    connect(m_slice, &SliceModel::frequencyChanged, this, [this](double) { updateFreqLabel(); });

    // Slice-level → shared DSP slider: when the active target's level
    // changes externally (radio echo, profile load, MIDI), update the
    // slider in-place without firing back into the slice.
    auto wireLevelEcho = [this](auto signal, DspLevelTarget tag) {
        connect(m_slice, signal, this, [this, tag](int v) {
            if (m_dspLevelTarget != tag || !m_dspLevelSlider) return;
            QSignalBlocker b(m_dspLevelSlider);
            m_dspLevelSlider->setValue(v);
            m_dspLevelValue->setText(QString::number(v));
        });
    };
    wireLevelEcho(&SliceModel::nrLevelChanged,   LvlNR);
    wireLevelEcho(&SliceModel::nbLevelChanged,   LvlNB);
    wireLevelEcho(&SliceModel::anfLevelChanged,  LvlAnf);
    wireLevelEcho(&SliceModel::nrlLevelChanged,  LvlNrl);
    wireLevelEcho(&SliceModel::nrsLevelChanged,  LvlNrs);
    wireLevelEcho(&SliceModel::nrfLevelChanged,  LvlNrf);
    wireLevelEcho(&SliceModel::anflLevelChanged, LvlAnfl);
    // Mode list (dynamic from radio)
    connect(m_slice, &SliceModel::modeListChanged, this, [this](const QStringList& modes) {
        if (modes.isEmpty()) return;          // keep static fallback list (#891)
        QSignalBlocker sb(m_modeCombo);
        QString cur = m_modeCombo->currentText();
        m_modeCombo->clear();
        m_modeCombo->addItems(modes);
#ifdef HAVE_RADE
        if (m_modeCombo->findText("RADE") < 0)
            m_modeCombo->addItem("RADE");
#endif
        int idx = m_modeCombo->findText(cur);
        if (idx >= 0) m_modeCombo->setCurrentIndex(idx);
    });
    // Populate now if already available
    if (!m_slice->modeList().isEmpty()) {
        QSignalBlocker sb(m_modeCombo);
        QString cur = m_modeCombo->currentText();
        m_modeCombo->clear();
        m_modeCombo->addItems(m_slice->modeList());
#ifdef HAVE_RADE
        if (m_modeCombo->findText("RADE") < 0)
            m_modeCombo->addItem("RADE");
#endif
        int idx = m_modeCombo->findText(cur);
        if (idx >= 0) m_modeCombo->setCurrentIndex(idx);
    }
    // Mode
    connect(m_slice, &SliceModel::modeChanged, this, [this](const QString& mode) {
        m_tabBtns[2]->setText(mode);  // update mode tab label
        updateModeTab();
        // Show/hide mode-specific DSP controls
        // Categorize by mode family (supports future/unknown modes)
        bool isRtty = (mode == "RTTY");
        bool isCw   = (mode == "CW" || mode == "CWL");
        bool isDig  = (mode == "DIGL" || mode == "DIGU" || mode == "NT");
        bool isFm   = (mode == "FM" || mode == "NFM" || mode == "DFM");
        bool isFdv  = mode.startsWith("FDV");  // FDVU, FDVM, etc.
        // Swap DSP tab label to OPT for FM modes
        m_tabBtns[1]->setText(isFm ? "OPT" : "DSP");
        m_rttyContainer->setVisible(isRtty);
        m_apfContainer->setVisible(isCw);
        m_digContainer->setVisible(isDig && !isFdv && mode != "NT");
        m_fmContainer->setVisible(isFm);
        if (isDig) {
            int off = (mode == "DIGL") ? m_slice->diglOffset() : m_slice->diguOffset();
            m_digOffsetLabel->setText(QString::number(off));
        }
        // CW: show APF, hide ANF/RNN/ANFL/ANFT
        // RTTY/DIG/FDV: hide ANF/ANFL/ANFT
        bool isVoice = !isRtty && !isCw && !isDig && !isFm && !isFdv;
        // Disable squelch in digital, RTTY, and CW modes
        // Digital/RTTY: audio feeds external decoders via DAX, SQL not meaningful
        //   and gates weak FSK signals (#2504)
        // CW: radio locks squelch on at fixed level, rejects changes
        bool sqlDisabled = isDig || isCw || isRtty;
        m_sqlBtn->setEnabled(!sqlDisabled);
        m_sqlSlider->setEnabled(!sqlDisabled);
        if (sqlDisabled && m_slice) {
            if (m_slice->squelchOn()) {
                m_savedSquelchOn = true;
                if (isDig || isRtty) {
                    m_slice->setSquelch(false, m_slice->squelchLevel());
                    QSignalBlocker sb(m_sqlBtn);
                    m_sqlBtn->setChecked(false);
                }
            }
        } else if (!sqlDisabled && m_slice && m_savedSquelchOn) {
            m_savedSquelchOn = false;
            m_slice->setSquelch(true, m_slice->squelchLevel());
            QSignalBlocker sb(m_sqlBtn);
            m_sqlBtn->setChecked(true);
        }
        m_apfBtn->setVisible(isCw);
        m_anfBtn->setVisible(isVoice);
        m_anflBtn->setVisible(isVoice);
        m_anftBtn->setVisible(isVoice);
        // Hide all DSP buttons in FM mode
        m_nrBtn->setVisible(!isFm);
        m_nbBtn->setVisible(!isFm);
        // NRL is available on 6000-series too (#2177)
        m_nrlBtn->setVisible(!isFm);
        // 8000-series-only firmware DSP filters (#2177)
        m_nrsBtn->setVisible(!isFm && m_hasExtendedDsp);
        m_rnnBtn->setVisible(!isCw && !isFm && m_hasExtendedDsp);
        m_nrfBtn->setVisible(!isFm && m_hasExtendedDsp);
        relayoutDspGrid();
        updateFilterLabel();
        if (m_tabStack->isVisible()) adjustSize();
    });
    // Filter
    connect(m_slice, &SliceModel::filterChanged, this, [this](int, int) {
        updateFilterLabel();
        updateFilterHighlight();
    });
    // Antennas
    connect(m_slice, &SliceModel::rxAntennaChanged, this, [this](const QString& ant) {
        m_updatingFromModel = true; m_rxAntBtn->setText(ant); m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::txAntennaChanged, this, [this](const QString& ant) {
        m_updatingFromModel = true; m_txAntBtn->setText(ant); m_updatingFromModel = false;
    });
    // TX slice — toggle between red (active TX) and grey (clickable to set TX)
    connect(m_slice, &SliceModel::txSliceChanged, this, [this](bool tx) {
        updateTxBadgeStyle(tx);
    });
    // Audio
    connect(m_slice, &SliceModel::audioGainChanged, this, [this](float g) {
        m_updatingFromModel = true;
        m_afGainSlider->setValue(static_cast<int>(g));
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::audioMuteChanged, this, [this](bool mute) {
        m_updatingFromModel = true;
        m_muteBtn->setChecked(mute);
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::audioPanChanged, this, [this](int pan) {
        m_updatingFromModel = true;
        m_panSlider->setValue(pan);
        m_updatingFromModel = false;
    });
    // Diversity sync
    {
        QSignalBlocker sb(m_divBtn);
        m_divBtn->setChecked(m_slice->diversity());
    }
    connect(m_slice, &SliceModel::diversityChanged, this, [this](bool on) {
        QSignalBlocker sb(m_divBtn);
        m_divBtn->setChecked(on);
        m_escPanel->setVisible(on && !m_slice->isDiversityChild());
        resize(sizeHint());
    });
    // ESC sync — phase is in radians, display as degrees
    {
        QSignalBlocker sb(m_escBtn);
        m_escBtn->setChecked(m_slice->escEnabled());
    }
    {
        float gain = m_slice->escGain();
        QSignalBlocker sb(m_escGainSlider);
        m_escGainSlider->setValue(static_cast<int>(gain * 100.0f));
        m_escGainLbl->setText(QString::number(gain, 'f', 2));
        m_phaseKnob->setGain(gain);
    }
    {
        float rad = m_slice->escPhaseShift();
        int deg = static_cast<int>(rad * 180.0f / M_PI) % 360;
        QSignalBlocker sb(m_escPhaseSlider);
        m_escPhaseSlider->setValue(deg / 5);
        m_escPhaseLbl->setText(QString::number(deg) + QChar(0x00B0));
        m_phaseKnob->setPhase(rad);
    }
    m_escPanel->setVisible(m_slice->diversity() && !m_slice->isDiversityChild());
    connect(m_slice, &SliceModel::escEnabledChanged, this, [this](bool on) {
        m_updatingFromModel = true;
        QSignalBlocker sb(m_escBtn);
        m_escBtn->setChecked(on);
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::escGainChanged, this, [this](float gain) {
        m_updatingFromModel = true;
        QSignalBlocker sb(m_escGainSlider);
        m_escGainSlider->setValue(static_cast<int>(gain * 100.0f));
        m_escGainLbl->setText(QString::number(gain, 'f', 2));
        m_phaseKnob->setGain(gain);
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::escPhaseShiftChanged, this, [this](float rad) {
        m_updatingFromModel = true;
        int deg = static_cast<int>(rad * 180.0f / M_PI) % 360;
        m_escPhaseSlider->setValue(deg / 5);
        m_escPhaseLbl->setText(QString::number(deg) + QChar(0x00B0));
        m_phaseKnob->setPhase(rad);
        m_updatingFromModel = false;
    });
    // DSP toggles
    auto connectDsp = [this](auto signal, QPushButton* btn) {
        connect(m_slice, signal, this, [this, btn](bool on) {
            m_updatingFromModel = true;
            QSignalBlocker sb(btn);
            btn->setChecked(on);
            m_updatingFromModel = false;
        });
    };
    // Leveled variant — also push/pop the shared DSP-level slider stack
    // when state changes arrive from the radio.  Without this the slider
    // is missing on launch for any DSP enabled in the radio's saved
    // profile until the user manually toggles it (#startup-slider).
    auto connectLeveledDsp = [this](auto signal, QPushButton* btn,
                                    DspLevelTarget tag) {
        connect(m_slice, signal, this, [this, btn, tag](bool on) {
            m_updatingFromModel = true;
            QSignalBlocker sb(btn);
            btn->setChecked(on);
            m_updatingFromModel = false;
            if (on) pushDspLevelTarget(tag);
            else    popDspLevelTarget(tag);
        });
    };
    connectLeveledDsp(&SliceModel::nbChanged,   m_nbBtn,   LvlNB);
    connectLeveledDsp(&SliceModel::nrChanged,   m_nrBtn,   LvlNR);
    connectLeveledDsp(&SliceModel::anfChanged,  m_anfBtn,  LvlAnf);
    connectLeveledDsp(&SliceModel::nrlChanged,  m_nrlBtn,  LvlNrl);
    connectLeveledDsp(&SliceModel::nrsChanged,  m_nrsBtn,  LvlNrs);
    connectDsp(&SliceModel::rnnChanged, m_rnnBtn);     // toggle-only, no level
    connectLeveledDsp(&SliceModel::nrfChanged,  m_nrfBtn,  LvlNrf);
    connectLeveledDsp(&SliceModel::anflChanged, m_anflBtn, LvlAnfl);
    connectDsp(&SliceModel::anftChanged, m_anftBtn);   // toggle-only, no level
    connectDsp(&SliceModel::apfChanged, m_apfBtn);     // own level row
    connect(m_slice, &SliceModel::apfLevelChanged, this, [this](int v) {
        m_updatingFromModel = true;
        m_apfSlider->setValue(v);
        m_apfValueLbl->setText(QString::number(v));
        m_updatingFromModel = false;
    });
    // Squelch
    connect(m_slice, &SliceModel::squelchChanged, this, [this](bool on, int level) {
        m_updatingFromModel = true;
        if (m_sqlBtn->isEnabled())
            m_sqlBtn->setChecked(on);
        m_sqlSlider->setValue(level);
        m_updatingFromModel = false;
    });
    // AGC
    connect(m_slice, &SliceModel::agcModeChanged, this, [this](const QString& mode) {
        m_updatingFromModel = true;
        QSignalBlocker sb(m_agcCmb);
        // Map protocol value to display text
        if (mode == "off") m_agcCmb->setCurrentText("Off");
        else if (mode == "slow") m_agcCmb->setCurrentText("Slow");
        else if (mode == "med") m_agcCmb->setCurrentText("Med");
        else if (mode == "fast") m_agcCmb->setCurrentText("Fast");
        updateAgcSliderFromSlice();
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::agcThresholdChanged, this, [this](int v) {
        Q_UNUSED(v);
        m_updatingFromModel = true;
        if (m_slice && m_slice->agcMode() != "off") updateAgcSliderFromSlice();
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::agcOffLevelChanged, this, [this](int v) {
        Q_UNUSED(v);
        m_updatingFromModel = true;
        if (m_slice && m_slice->agcMode() == "off") updateAgcSliderFromSlice();
        m_updatingFromModel = false;
    });
    // RIT/XIT
    connect(m_slice, &SliceModel::ritChanged, this, [this](bool on, int hz) {
        m_updatingFromModel = true;
        QSignalBlocker sb(m_ritBtn);
        m_ritBtn->setChecked(on);
        m_ritLabel->setText(QString("%1%2 Hz").arg(hz >= 0 ? "+" : "").arg(hz));
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::xitChanged, this, [this](bool on, int hz) {
        m_updatingFromModel = true;
        QSignalBlocker sb(m_xitBtn);
        m_xitBtn->setChecked(on);
        m_xitLabel->setText(QString("%1%2 Hz").arg(hz >= 0 ? "+" : "").arg(hz));
        m_updatingFromModel = false;
    });
    // FM controls
    connect(m_slice, &SliceModel::fmToneModeChanged, this, [this](const QString& mode) {
        m_updatingFromModel = true;
        QSignalBlocker sb(m_fmToneModeCmb);
        int idx = m_fmToneModeCmb->findData(mode);
        if (idx >= 0) m_fmToneModeCmb->setCurrentIndex(idx);
        m_fmToneValueCmb->setEnabled(mode == "ctcss_tx");
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::fmToneValueChanged, this, [this](const QString& val) {
        m_updatingFromModel = true;
        QSignalBlocker sb(m_fmToneValueCmb);
        int idx = m_fmToneValueCmb->findData(val);
        if (idx >= 0) m_fmToneValueCmb->setCurrentIndex(idx);
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::repeaterOffsetDirChanged, this, [this](const QString& dir) {
        m_updatingFromModel = true;
        QSignalBlocker b1(m_fmOffsetDown), b2(m_fmSimplexBtn), b3(m_fmOffsetUp);
        m_fmOffsetDown->setChecked(dir == "down");
        m_fmSimplexBtn->setChecked(dir == "simplex");
        m_fmOffsetUp->setChecked(dir == "up");
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::fmRepeaterOffsetFreqChanged, this, [this](double mhz) {
        m_updatingFromModel = true;
        QSignalBlocker sb(m_fmOffsetSpin);
        m_fmOffsetSpin->setValue(mhz);
        m_updatingFromModel = false;
    });
    // DIG offset
    connect(m_slice, &SliceModel::diglOffsetChanged, this, [this](int hz) {
        if (m_slice && m_slice->mode() == "DIGL")
            m_digOffsetLabel->setText(QString::number(hz));
    });
    connect(m_slice, &SliceModel::diguOffsetChanged, this, [this](int hz) {
        if (m_slice && m_slice->mode() == "DIGU")
            m_digOffsetLabel->setText(QString::number(hz));
    });
    // RTTY Mark/Shift
    connect(m_slice, &SliceModel::rttyMarkChanged, this, [this](int hz) {
        m_markLabel->setText(QString::number(hz));
    });
    connect(m_slice, &SliceModel::rttyShiftChanged, this, [this](int hz) {
        m_shiftLabel->setText(QString::number(hz));
    });
    // DAX
    connect(m_slice, &SliceModel::daxChannelChanged, this, [this](int ch) {
        m_updatingFromModel = true;
        QSignalBlocker sb(m_daxCmb);
        m_daxCmb->setCurrentIndex(ch);
        m_updatingFromModel = false;
    });
    connect(m_slice, &SliceModel::lockedChanged, this, [this](bool locked) {
        QSignalBlocker b(m_lockVfoBtn);
        m_lockVfoBtn->setChecked(locked);
        m_lockVfoBtn->setText(locked ? "\xF0\x9F\x94\x92" : "\xF0\x9F\x94\x93");
    });

    // Restore collapsed state from AppSettings
    {
        auto& s = AppSettings::instance();
        bool savedCollapsed = s.value(
            QString("SliceFlagCollapsed_%1").arg(m_slice->sliceId()), "False").toString() == "True";
        if (savedCollapsed != m_collapsed) {
            setCollapsed(savedCollapsed);
        }
    }

    syncFromSlice();
}

void VfoWidget::updateTxBadgeStyle(bool isTx)
{
    if (isTx) {
        m_txBadge->setStyleSheet(
            "QPushButton { background: #cc0000; color: #ffffff; border: none; "
            "border-radius: 2px; font-size: 12px; font-weight: bold; padding: 0; }"
            "QPushButton:hover { background: #ff2222; }");
    } else {
        m_txBadge->setStyleSheet(
            "QPushButton { background: #404040; color: #808080; border: none; "
            "border-radius: 2px; font-size: 12px; font-weight: bold; padding: 0; }"
            "QPushButton:hover { background: #606060; color: #c0c0c0; }");
    }
}

void VfoWidget::updateSplitBadge(bool isTxSlice, bool isRxSplit)
{
    if (isTxSlice) {
        // TX slice in split pair — show SWAP button
        m_splitBadge->show();
        m_splitBadge->setText("SWAP");
        m_splitBadge->setStyleSheet(
            "QPushButton { background: #204060; color: #80c0ff; border: none; "
            "border-radius: 2px; font-size: 11px; font-weight: bold; "
            "padding: 0px 3px; }"
            "QPushButton:hover { background: #306080; color: #ffffff; }");
    } else if (isRxSplit) {
        // RX slice that initiated split — red badge, full opacity
        m_splitBadge->show();
        m_splitBadge->setStyleSheet(
            "QPushButton { background: #cc0000; color: #ffffff; border: none; "
            "border-radius: 2px; font-size: 11px; font-weight: bold; "
            "padding: 0px 3px; }"
            "QPushButton:hover { background: #ee2222; }");
    } else {
        // Split not active — ghosted on all slices
        m_splitBadge->show();
        m_splitBadge->setStyleSheet(
            "QPushButton { background: transparent; border: none; "
            "color: rgba(255,255,255,40); font-size: 11px; font-weight: bold; "
            "padding: 0px 3px; }"
            "QPushButton:hover { color: rgba(255,255,255,80); }");
    }
}

void VfoWidget::setRecordOn(bool on)
{
    if (m_recordBtn) {
        QSignalBlocker sb(m_recordBtn);
        m_recordBtn->setChecked(on);
    }
    if (on)
        m_recordPulse->start();
    else {
        m_recordPulse->stop();
        // Restore normal checked style
        if (m_recordBtn)
            m_recordBtn->setStyleSheet(
                "QPushButton { background: rgba(255,255,255,30); border: none; "
                "border-radius: 10px; font-size: 11px; padding: 0; color: #c06060; }"
                "QPushButton:checked { color: #ff2020; background: rgba(255,50,50,110); }"
                "QPushButton:hover { background: rgba(255,255,255,60); }");
    }
}

void VfoWidget::setPlayOn(bool on)
{
    if (m_playBtn) {
        QSignalBlocker sb(m_playBtn);
        m_playBtn->setChecked(on);
    }
}

void VfoWidget::setPlayEnabled(bool enabled)
{
    if (m_playBtn)
        m_playBtn->setEnabled(enabled);
}

void VfoWidget::beginDirectEntry(QString source)
{
    m_directEntrySource = source;
    if (m_slice) {
        m_freqEdit->setText(QString::number(m_slice->frequency(), 'f', 6));
        m_freqEdit->selectAll();
    }
    m_freqStack->setCurrentIndex(1);
    raise();
    m_freqEdit->setFocus(Qt::ShortcutFocusReason);
    QTimer::singleShot(0, m_freqEdit, [edit = m_freqEdit]() {
        if (!edit) return;
        edit->setFocus(Qt::ShortcutFocusReason);
        edit->selectAll();
    });
}

bool VfoWidget::cancelDirectEntry()
{
    if (!m_freqStack || !m_freqEdit || m_freqStack->currentIndex() != 1)
        return false;

    m_directEntrySource = "vfo-direct-entry";
    if (m_slice)
        m_freqEdit->setText(QString::number(m_slice->frequency(), 'f', 6));
    m_freqStack->setCurrentIndex(0);
    m_freqEdit->clearFocus();
    return true;
}

void VfoWidget::syncFromSlice()
{
    if (!m_slice) return;
    m_updatingFromModel = true;

    m_rxAntBtn->setText(m_slice->rxAntenna());
    m_txAntBtn->setText(m_slice->txAntenna());
    updateTxBadgeStyle(m_slice->isTxSlice());
    {
        QSignalBlocker b(m_lockVfoBtn);
        m_lockVfoBtn->setChecked(m_slice->isLocked());
        m_lockVfoBtn->setText(m_slice->isLocked() ? "\xF0\x9F\x94\x92" : "\xF0\x9F\x94\x93");
    }
    const char letters[] = "ABCDEFGH";
    int id = m_slice->sliceId();
    m_sliceBadge->setText(QString(QChar(id >= 0 && id < 8 ? letters[id] : '?')));
    // Color-code the slice badge to match the spectrum overlay colors
    m_sliceBadge->setStyleSheet(
        QString("QLabel { background: %1; color: #000000; "
                "border-radius: 3px; font-weight: bold; font-size: 11px; }")
            .arg(SliceColorManager::instance().hexActive(id)));
    updateFreqLabel();
    updateFilterLabel();

    // Mode tab
    m_tabBtns[2]->setText(m_slice->mode());
    updateModeTab();

    // Audio
    m_afGainSlider->setValue(static_cast<int>(m_slice->audioGain()));
    m_panSlider->setValue(m_slice->audioPan());
    {
        QSignalBlocker sb(m_muteBtn);
        bool muted = m_slice->audioMute();
        m_muteBtn->setChecked(muted);
        m_muteBtn->setText(muted ? QString::fromUtf8("AF  \xF0\x9F\x94\x87")
                                 : QString::fromUtf8("AF  \xF0\x9F\x94\x8A"));
        m_tabBtns[0]->setText(muted ? QString::fromUtf8("\xF0\x9F\x94\x87")
                                    : QString::fromUtf8("\xF0\x9F\x94\x8A"));
    }
    {
        QSignalBlocker b1(m_sqlBtn), b2(m_sqlSlider);
        m_sqlBtn->setChecked(m_slice->squelchOn());
        m_sqlSlider->setValue(m_slice->squelchLevel());
    }
    {
        QSignalBlocker sb(m_agcCmb);
        const QString& mode = m_slice->agcMode();
        if (mode == "off") m_agcCmb->setCurrentText("Off");
        else if (mode == "slow") m_agcCmb->setCurrentText("Slow");
        else if (mode == "med") m_agcCmb->setCurrentText("Med");
        else if (mode == "fast") m_agcCmb->setCurrentText("Fast");
    }
    updateAgcSliderFromSlice();

    // ESC (diversity beamforming) — phase in radians, display as degrees
    {
        QSignalBlocker sb(m_escBtn);
        m_escBtn->setChecked(m_slice->escEnabled());
    }
    {
        float gain = m_slice->escGain();
        QSignalBlocker sb(m_escGainSlider);
        m_escGainSlider->setValue(static_cast<int>(gain * 100.0f));
        m_escGainLbl->setText(QString::number(gain, 'f', 2));
        m_phaseKnob->setGain(gain);
    }
    {
        float rad = m_slice->escPhaseShift();
        int deg = static_cast<int>(rad * 180.0f / M_PI) % 360;
        QSignalBlocker sb(m_escPhaseSlider);
        m_escPhaseSlider->setValue(deg / 5);
        m_escPhaseLbl->setText(QString::number(deg) + QChar(0x00B0));
        m_phaseKnob->setPhase(rad);
    }
    m_escPanel->setVisible(m_slice->diversity() && !m_slice->isDiversityChild());

    // DSP
    auto syncDsp = [](QPushButton* btn, bool on) {
        QSignalBlocker sb(btn); btn->setChecked(on);
    };
    syncDsp(m_nbBtn,  m_slice->nbOn());
    syncDsp(m_nrBtn,  m_slice->nrOn());
    syncDsp(m_anfBtn, m_slice->anfOn());
    syncDsp(m_nrlBtn, m_slice->nrlOn());
    syncDsp(m_nrsBtn, m_slice->nrsOn());
    syncDsp(m_rnnBtn, m_slice->rnnOn());
    syncDsp(m_nrfBtn, m_slice->nrfOn());
    syncDsp(m_anflBtn, m_slice->anflOn());
    syncDsp(m_anftBtn, m_slice->anftOn());
    syncDsp(m_apfBtn, m_slice->apfOn());

    // Shared DSP-level slider — pick the highest-priority enabled DSP.
    refreshDspLevelTarget();

    // RIT/XIT
    {
        QSignalBlocker sb1(m_ritBtn), sb2(m_xitBtn);
        m_ritBtn->setChecked(m_slice->ritOn());
        m_xitBtn->setChecked(m_slice->xitOn());
    }
    m_ritLabel->setText(QString("%1%2 Hz").arg(m_slice->ritFreq() >= 0 ? "+" : "").arg(m_slice->ritFreq()));
    m_xitLabel->setText(QString("%1%2 Hz").arg(m_slice->xitFreq() >= 0 ? "+" : "").arg(m_slice->xitFreq()));

    // RTTY
    bool isRtty = (m_slice->mode() == "RTTY");
    m_markLabel->setText(QString::number(m_slice->rttyMark()));
    m_shiftLabel->setText(QString::number(m_slice->rttyShift()));
    m_rttyContainer->setVisible(isRtty);
    bool isCw = (m_slice->mode() == "CW" || m_slice->mode() == "CWL");
    bool isDig = (m_slice->mode() == "DIGL" || m_slice->mode() == "DIGU" || m_slice->mode() == "NT");
    bool isFm = (m_slice->mode() == "FM" || m_slice->mode() == "NFM");
    m_tabBtns[1]->setText(isFm ? "OPT" : "DSP");
    m_apfBtn->setVisible(isCw);
    m_anfBtn->setVisible(!isRtty && !isCw && !isDig && !isFm);
    m_anflBtn->setVisible(!isRtty && !isCw && !isDig && !isFm);
    m_anftBtn->setVisible(!isRtty && !isCw && !isDig && !isFm);
    m_nrBtn->setVisible(!isFm);
    m_nbBtn->setVisible(!isFm);
    // NRL is available on 6000-series too (#2177)
    m_nrlBtn->setVisible(!isFm);
    // 8000-series-only firmware DSP filters (#2177)
    m_nrsBtn->setVisible(!isFm && m_hasExtendedDsp);
    m_rnnBtn->setVisible(!isCw && !isFm && m_hasExtendedDsp);
    m_nrfBtn->setVisible(!isFm && m_hasExtendedDsp);
    m_apfContainer->setVisible(isCw);
    m_digContainer->setVisible(isDig && m_slice->mode() != "NT");
    m_fmContainer->setVisible(isFm);
    // CW: radio locks squelch on at fixed level; Digital: not meaningful
    m_sqlBtn->setEnabled(!isDig && !isCw);
    m_sqlSlider->setEnabled(!isDig && !isCw);
    if (isFm) {
        QSignalBlocker b1(m_fmToneModeCmb), b2(m_fmToneValueCmb), b3(m_fmOffsetSpin);
        int tmIdx = m_fmToneModeCmb->findData(m_slice->fmToneMode());
        if (tmIdx >= 0) m_fmToneModeCmb->setCurrentIndex(tmIdx);
        m_fmToneValueCmb->setEnabled(m_slice->fmToneMode() == "ctcss_tx");
        int tvIdx = m_fmToneValueCmb->findData(m_slice->fmToneValue());
        if (tvIdx >= 0) m_fmToneValueCmb->setCurrentIndex(tvIdx);
        m_fmOffsetSpin->setValue(m_slice->fmRepeaterOffsetFreq());
        QSignalBlocker b4(m_fmOffsetDown), b5(m_fmSimplexBtn), b6(m_fmOffsetUp);
        const QString& dir = m_slice->repeaterOffsetDir();
        m_fmOffsetDown->setChecked(dir == "down");
        m_fmSimplexBtn->setChecked(dir == "simplex");
        m_fmOffsetUp->setChecked(dir == "up");
    }
    if (isDig) {
        int off = (m_slice->mode() == "DIGL") ? m_slice->diglOffset() : m_slice->diguOffset();
        m_digOffsetLabel->setText(QString::number(off));
    }
    relayoutDspGrid();

    // APF level
    {
        QSignalBlocker sb(m_apfSlider);
        m_apfSlider->setValue(m_slice->apfLevel());
        m_apfValueLbl->setText(QString::number(m_slice->apfLevel()));
    }

    // DAX
    {
        QSignalBlocker sb(m_daxCmb);
        m_daxCmb->setCurrentIndex(m_slice->daxChannel());
    }

    m_updatingFromModel = false;
}

void VfoWidget::updateFreqLabel()
{
    if (!m_slice) return;
    long long hz = static_cast<long long>(std::round(m_slice->frequency() * 1e6));
    int mhzPart = static_cast<int>(hz / 1000000);
    int khzPart = static_cast<int>((hz / 1000) % 1000);
    int hzPart  = static_cast<int>(hz % 1000);
    QString freqText = QString("%1.%2.%3")
        .arg(mhzPart)
        .arg(khzPart, 3, 10, QChar('0'))
        .arg(hzPart, 3, 10, QChar('0'));
    m_freqLabel->setText(freqText);

    // Keep collapsed frequency label in sync
    if (m_collapsed && m_collapsedFreqLabel) {
        m_collapsedFreqLabel->setText(freqText);
        m_collapsedFreqLabel->adjustSize();
    }
}

void VfoWidget::updateFilterLabel()
{
    if (!m_slice) return;
    // Single source of truth with the RX applet's filter readout to keep both
    // labels in sync — they previously drifted (#794, #1225, #2197).
    m_filterWidthLbl->setText(RxApplet::formatFilterWidth(
        m_slice->filterLow(), m_slice->filterHigh(), m_slice->mode()));
}

void VfoWidget::relayoutDspGrid()
{
    // Remove all widgets from the grid (without deleting them)
    QPushButton* all[] = {m_nrBtn, m_nbBtn, m_anfBtn, m_apfBtn, m_nrlBtn,
                          m_nrsBtn, m_rnnBtn, m_nrfBtn, m_anflBtn, m_anftBtn};
    for (auto* btn : all)
        m_dspGrid->removeWidget(btn);
    if (m_aetherDspBtn)
        m_dspGrid->removeWidget(m_aetherDspBtn);
    if (m_aetherVoiceBtn)
        m_dspGrid->removeWidget(m_aetherVoiceBtn);

    // Re-add only non-hidden buttons in 4-column rows
    int col = 0, row = 0;
    for (auto* btn : all) {
        if (!btn->isHidden()) {
            m_dspGrid->addWidget(btn, row, col);
            if (++col >= 4) { col = 0; ++row; }
        }
    }
    // Client-side launchers: ADSP (1 col) + AetherVoice (2 cols spanning
    // the rightmost 2 columns) live on the same row.  If ADSP would land
    // beyond col 1, wrap the pair to a fresh row first so AetherVoice
    // always occupies cols 2-3.
    if (m_aetherDspBtn && m_aetherVoiceBtn) {
        if (col > 1) { col = 0; ++row; }
        m_dspGrid->addWidget(m_aetherDspBtn, row, col);
        m_dspGrid->addWidget(m_aetherVoiceBtn, row, 2, 1, 2);
    }
}

// ── Shared DSP level slider ───────────────────────────────────────────────────

void VfoWidget::pushDspLevelTarget(DspLevelTarget t)
{
    if (t == LvlNone) return;
    m_dspLevelStack.removeAll(t);
    m_dspLevelStack.append(t);
    setDspLevelTarget(t);
}

void VfoWidget::popDspLevelTarget(DspLevelTarget t)
{
    m_dspLevelStack.removeAll(t);
    if (m_dspLevelTarget == t) {
        if (!m_dspLevelStack.isEmpty())
            setDspLevelTarget(m_dspLevelStack.last());
        else
            setDspLevelTarget(LvlNone);
    }
}

void VfoWidget::setDspLevelTarget(DspLevelTarget t)
{
    m_dspLevelTarget = t;
    if (!m_dspLevelRow) return;
    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(m_dspLevelRow->graphicsEffect());
    if (t == LvlNone || !m_slice) {
        if (eff) eff->setOpacity(0.0);
        return;
    }
    int level = 0;
    QString name;
    switch (t) {
        case LvlNR:   level = m_slice->nrLevel();   name = "NR";   break;
        case LvlNB:   level = m_slice->nbLevel();   name = "NB";   break;
        case LvlAnf:  level = m_slice->anfLevel();  name = "ANF";  break;
        case LvlNrl:  level = m_slice->nrlLevel();  name = "NRL";  break;
        case LvlNrs:  level = m_slice->nrsLevel();  name = "NRS";  break;
        case LvlNrf:  level = m_slice->nrfLevel();  name = "NRF";  break;
        case LvlAnfl: level = m_slice->anflLevel(); name = "ANFL"; break;
        case LvlNone: return;
    }
    m_dspLevelLabel->setText(name);
    {
        QSignalBlocker b(m_dspLevelSlider);
        m_dspLevelSlider->setValue(level);
    }
    m_dspLevelValue->setText(QString::number(level));
    if (eff) eff->setOpacity(1.0);
}

void VfoWidget::refreshDspLevelTarget()
{
    m_dspLevelStack.clear();
    if (!m_slice) {
        setDspLevelTarget(LvlNone);
        return;
    }
    // Seed the activation stack from current slice state in priority
    // order (NR, NB, ANF, NRL, NRS, NRF, ANFL) — we have no record of
    // the radio's actual click order, so use the priority sequence as
    // a deterministic fallback.  Most recent (last in stack) becomes
    // the active target.
    auto isOn = [this](DspLevelTarget t) {
        if (!m_slice) return false;
        switch (t) {
            case LvlNR:   return m_slice->nrOn();
            case LvlNB:   return m_slice->nbOn();
            case LvlAnf:  return m_slice->anfOn();
            case LvlNrl:  return m_slice->nrlOn();
            case LvlNrs:  return m_slice->nrsOn();
            case LvlNrf:  return m_slice->nrfOn();
            case LvlAnfl: return m_slice->anflOn();
            case LvlNone: return false;
        }
        return false;
    };
    for (auto t : { LvlNR, LvlNB, LvlAnf, LvlNrl, LvlNrs, LvlNrf, LvlAnfl }) {
        if (isOn(t)) m_dspLevelStack.append(t);
    }
    if (m_dspLevelStack.isEmpty())
        setDspLevelTarget(LvlNone);
    else
        setDspLevelTarget(m_dspLevelStack.last());
}

// ── Mode tab helpers ──────────────────────────────────────────────────────────

struct ModeFilterPresets {
    QVector<int> filterWidths;
};

static const ModeFilterPresets& filterPresetsFor(const QString& mode)
{
    // From docs/vfo_mode_filters.csv — 8 presets per mode, 4x2 grid
    static const ModeFilterPresets usb{{1800, 2100, 2400, 2700, 2900, 3300, 4000, 6000}};
    static const ModeFilterPresets am {{5600, 6000, 8000, 10000, 12000, 14000, 16000, 20000}};
    static const ModeFilterPresets cw {{50, 100, 250, 400}};
    static const ModeFilterPresets dig{{100, 300, 600, 1000, 1500, 2000, 3000, 6000}};
    static const ModeFilterPresets rtty{{250, 300, 350, 400, 500, 1000, 1500, 3000}};
    static const ModeFilterPresets dfm{{6000, 8000, 10000, 12000, 14000, 16000, 18000, 20000}};
    static const ModeFilterPresets fm{{}};

    if (mode == "USB" || mode == "LSB") return usb;
    if (mode == "AM" || mode == "SAM") return am;
    if (mode == "CW") return cw;
    if (mode == "DIGU" || mode == "DIGL" || mode == "NT") return dig;
    if (mode == "RTTY") return rtty;
    if (mode == "DFM") return dfm;
    if (mode == "FM" || mode == "NFM") return fm;
    return usb;
}

void VfoWidget::updateModeTab()
{
    if (!m_slice) return;
    const QString& cur = m_slice->mode();

    // Sync combo
    {
        QSignalBlocker sb(m_modeCombo);
        int idx = m_modeCombo->findText(cur);
        if (idx >= 0) m_modeCombo->setCurrentIndex(idx);
    }

    // Update quick-mode button labels and active state
    updateQuickModeButtons();

    // Load custom filter presets from AppSettings, fall back to defaults.
    // Storage: "width,width,lo:hi,width,..." — "lo:hi" entries override
    // mode-rule recompute and apply explicit edges directly. (#2259)
    QString fkey = QStringLiteral("FilterPresets_%1").arg(cur);
    QString saved = AppSettings::instance().value(fkey, "").toString();
    m_filterWidths.clear();
    m_filterCustomLo.clear();
    m_filterCustomHi.clear();
    if (!saved.isEmpty()) {
        for (const auto& s : saved.split(',', Qt::SkipEmptyParts)) {
            if (s.contains(':')) {
                const auto parts = s.split(':');
                if (parts.size() != 2) continue;
                bool okLo, okHi;
                int lo = parts[0].toInt(&okLo);
                int hi = parts[1].toInt(&okHi);
                if (!okLo || !okHi || hi <= lo) continue;
                m_filterWidths.append(hi - lo);
                m_filterCustomLo.append(lo);
                m_filterCustomHi.append(hi);
            } else {
                bool ok;
                int w = s.toInt(&ok);
                if (!ok || w <= 0) continue;
                m_filterWidths.append(w);
                m_filterCustomLo.append(INT_MIN);
                m_filterCustomHi.append(INT_MIN);
            }
        }
    }
    if (m_filterWidths.isEmpty()) {
        m_filterWidths = filterPresetsFor(cur).filterWidths;
        m_filterCustomLo.fill(INT_MIN, m_filterWidths.size());
        m_filterCustomHi.fill(INT_MIN, m_filterWidths.size());
    }
    rebuildFilterButtons();
}

void VfoWidget::updateQuickModeButtons()
{
    const QString cur = m_slice ? m_slice->mode() : QString();

    for (int i = 0; i < 3; ++i) {
        if (!m_quickModeBtns[i]) continue;
        const QString& assign = m_quickModeAssign[i];

        // Determine label and active state
        QString label = assign;
        bool active = false;
        if (assign == "SSB") {
            label = (cur == "LSB") ? "LSB" : "USB";
            active = (cur == "USB" || cur == "LSB");
        } else if (assign == "DIG") {
            label = (cur == "DIGL") ? "DIGL" : "DIGU";
            active = (cur == "DIGU" || cur == "DIGL");
        } else {
            active = (cur == assign);
        }

        m_quickModeBtns[i]->setText(label);
        QSignalBlocker sb(m_quickModeBtns[i]);
        m_quickModeBtns[i]->setChecked(active);
    }
}

QString VfoWidget::formatFilterLabel(int hz)
{
    if (hz >= 1000) return QString("%1K").arg(hz / 1000.0, 0, 'f', (hz % 1000) ? 1 : 0);
    return QString::number(hz);
}

void VfoWidget::updateAgcSliderFromSlice()
{
    if (!m_slice || !m_agcTSlider || !m_agcValueLbl) return;

    const bool agcOff = (m_slice->agcMode() == "off");
    const int value = agcOff ? m_slice->agcOffLevel() : m_slice->agcThreshold();

    QSignalBlocker blocker(m_agcTSlider);
    m_agcTSlider->setValue(value);
    m_agcTSlider->setToolTip(agcOff
        ? QString("AGC Off Level: %1").arg(value)
        : QString("AGC Threshold: %1").arg(value));
    m_agcTSlider->setAccessibleName(agcOff ? "AGC off level" : "AGC threshold");
    m_agcValueLbl->setText(QString::number(value));
}

void VfoWidget::rebuildFilterButtons()
{
    for (auto* btn : m_filterBtns) delete btn;
    m_filterBtns.clear();
    // Remove autotune row if it exists (re-added for CW). Delete the
    // container — its children ("Autotune:" label, buttons) go with it.
    if (m_autotuneContainer) {
        delete m_autotuneContainer;
        m_autotuneContainer = nullptr;
        m_autotuneOnceBtn = nullptr;
        m_autotuneLoopBtn = nullptr;
        m_zeroBeatBtn = nullptr;
    }
    // Remove marker-style buttons if they exist (re-added for CW only, #1526)
    if (m_markerThicknessBtn) { delete m_markerThicknessBtn; m_markerThicknessBtn = nullptr; }
    if (m_edgesBtn)           { delete m_edgesBtn;           m_edgesBtn = nullptr; }

    for (int i = 0; i < m_filterWidths.size(); ++i) {
        const int w = m_filterWidths[i];
        auto* btn = new QPushButton(formatFilterLabel(w));
        btn->setCheckable(true);
        btn->setFixedHeight(26);
        btn->setStyleSheet(kModeBtn);
        connect(btn, &QPushButton::clicked, this, [this, i](bool) {
            if (!m_slice) return;
            if (m_filterCustomLo[i] != INT_MIN) {
                // Custom edges from right-click → "Set Custom Edges..."
                m_slice->setFilterWidth(m_filterCustomLo[i], m_filterCustomHi[i]);
            } else {
                applyFilterPreset(m_filterWidths[i]);
            }
        });

        // Right-click to customize this preset
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QPushButton::customContextMenuRequested, this, [this, i, btn](const QPoint& pos) {
            QMenu menu;
            menu.addAction("Set Custom Edges...", [this, i] {
                if (!m_slice) return;
                QDialog dlg(this);
                dlg.setWindowTitle("Set Custom Filter Edges");
                auto* form = new QFormLayout(&dlg);
                auto* loSpin = new QSpinBox(&dlg);
                auto* hiSpin = new QSpinBox(&dlg);
                loSpin->setRange(-20000, 20000);
                hiSpin->setRange(-20000, 20000);
                loSpin->setSingleStep(50);
                hiSpin->setSingleStep(50);
                loSpin->setSuffix(" Hz");
                hiSpin->setSuffix(" Hz");
                int curLo = m_filterCustomLo[i] != INT_MIN
                                ? m_filterCustomLo[i] : m_slice->filterLow();
                int curHi = m_filterCustomHi[i] != INT_MIN
                                ? m_filterCustomHi[i] : m_slice->filterHigh();
                loSpin->setValue(curLo);
                hiSpin->setValue(curHi);
                form->addRow("Low edge:", loSpin);
                form->addRow("High edge:", hiSpin);
                auto* btns = new QDialogButtonBox(
                    QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
                QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
                QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
                form->addRow(btns);
                if (dlg.exec() != QDialog::Accepted) return;
                int lo = loSpin->value();
                int hi = hiSpin->value();
                if (hi <= lo) return;
                m_filterCustomLo[i] = lo;
                m_filterCustomHi[i] = hi;
                m_filterWidths[i] = hi - lo;
                saveFilterPresets();
                rebuildFilterButtons();
                m_slice->setFilterWidth(lo, hi);
            });
            menu.addAction("Reset to Default", [this, i] {
                if (!m_slice) return;
                const auto& factory = filterPresetsFor(m_slice->mode()).filterWidths;
                if (i >= factory.size()) return;
                m_filterWidths[i] = factory[i];
                m_filterCustomLo[i] = INT_MIN;
                m_filterCustomHi[i] = INT_MIN;
                saveFilterPresets();
                rebuildFilterButtons();
                applyFilterPreset(m_filterWidths[i]);
            });
            menu.exec(btn->mapToGlobal(pos));
        });

        m_filterBtns.append(btn);
        m_filterGrid->addWidget(btn, i / 4, i % 4);
    }

    // Per-slice VFO marker style row: Thin/Thick line + Edges/Hide filter
    // edges (#1526). Shown in every mode — narrow filters aren't CW-exclusive
    // (narrow DIGL, RTTY, CWL, etc. all benefit from hiding the overlapping
    // edge lines), and users who want a thicker center marker on any mode
    // can set it. Cleanup in the rebuildFilterButtons header ensures we
    // don't accumulate duplicate rows on mode change.
    {
        int row = (m_filterWidths.size() + 3) / 4;

        // Marker: cycle through Off → 1 px → 3 px on each click.  Label
        // reflects the current state.  Off removes the center line and the
        // top triangle entirely so only the passband bracket is shown.
        const QString markerLabel = (m_markerWidth == 0)
            ? QStringLiteral("Marker: Off")
            : QStringLiteral("Marker: %1px").arg(m_markerWidth);
        m_markerThicknessBtn = new QPushButton(markerLabel);
        m_markerThicknessBtn->setFixedHeight(26);
        m_markerThicknessBtn->setStyleSheet(kModeBtn);
        m_markerThicknessBtn->setToolTip("Cycle VFO marker: Off → 1 px → 3 px");
        connect(m_markerThicknessBtn, &QPushButton::clicked, this, [this]() {
            int next = (m_markerWidth == 0) ? 1 : (m_markerWidth == 1) ? 3 : 0;
            setMarkerWidth(next);
        });
        m_filterGrid->addWidget(m_markerThicknessBtn, row, 0, 1, 2);

        // Filter edge lines: checkable on/off.  Checked = edges shown.
        m_edgesBtn = new QPushButton("Filter Edge");
        m_edgesBtn->setCheckable(true);
        m_edgesBtn->setChecked(!m_filterEdgesHidden);
        m_edgesBtn->setFixedHeight(26);
        m_edgesBtn->setStyleSheet(kModeBtn);
        m_edgesBtn->setToolTip("Show filter edge lines");
        connect(m_edgesBtn, &QPushButton::toggled, this, [this](bool on) {
            setFilterEdgesHidden(!on);
        });
        m_filterGrid->addWidget(m_edgesBtn, row, 2, 1, 2);
    }

    // Add CW autotune row spanning all 4 columns when in CW mode
    if (m_slice && (m_slice->mode() == "CW" || m_slice->mode() == "CWL")) {
        int row = (m_filterWidths.size() + 3) / 4 + 1;

        m_autotuneContainer = new QWidget;
        auto* container = m_autotuneContainer;
        auto* hbox = new QHBoxLayout(container);
        hbox->setContentsMargins(0, 0, 0, 0);
        hbox->setSpacing(4);

        auto* label = new QLabel("Autotune:");
        label->setStyleSheet("QLabel { color: #8898a8; font-size: 11px; }");
        hbox->addWidget(label);

        const QString btnStyle =
            "QPushButton { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050; "
            "border-radius: 3px; font-size: 10px; padding: 0 8px; }"
            "QPushButton:hover { background: #253545; }"
            "QPushButton:pressed { background: #00607a; }"
            "QPushButton:checked { background: #00607a; color: #e0f0ff; border-color: #00b4d8; }";

        if (m_hasSmartSdrPlus) {
            m_autotuneOnceBtn = new QPushButton("Once");
            m_autotuneOnceBtn->setFixedHeight(26);
            m_autotuneOnceBtn->setStyleSheet(btnStyle);
            connect(m_autotuneOnceBtn, &QPushButton::clicked, this, [this]() {
                emit autotuneOnceRequested();
            });
            hbox->addWidget(m_autotuneOnceBtn, 1);

            m_autotuneLoopBtn = new QPushButton("Loop");
            m_autotuneLoopBtn->setCheckable(true);
            m_autotuneLoopBtn->setFixedHeight(26);
            m_autotuneLoopBtn->setStyleSheet(btnStyle);
            connect(m_autotuneLoopBtn, &QPushButton::toggled, this, [this](bool on) {
                emit autotuneRequested(on);
            });
            hbox->addWidget(m_autotuneLoopBtn, 1);
        } else {
            m_zeroBeatBtn = new QPushButton("Zero Beat");
            m_zeroBeatBtn->setFixedHeight(26);
            m_zeroBeatBtn->setStyleSheet(btnStyle);
            connect(m_zeroBeatBtn, &QPushButton::clicked, this, [this]() {
                emit zeroBeatRequested();
            });
            hbox->addWidget(m_zeroBeatBtn, 1);
        }

        m_filterGrid->addWidget(container, row, 0, 1, 4);
    }

    updateFilterHighlight();
}

void VfoWidget::updateFilterHighlight()
{
    if (!m_slice) return;

    // Reload presets from AppSettings in case RxApplet changed them.
    // Format mirrors updateModeTab(): "width" or "lo:hi" entries (#2259).
    const QString key = QStringLiteral("FilterPresets_%1").arg(m_slice->mode());
    const QString saved = AppSettings::instance().value(key, "").toString();
    if (!saved.isEmpty()) {
        QVector<int> loadedWidths;
        QVector<int> loadedLo;
        QVector<int> loadedHi;
        for (const auto& s : saved.split(',', Qt::SkipEmptyParts)) {
            if (s.contains(':')) {
                const auto parts = s.split(':');
                if (parts.size() != 2) continue;
                bool okLo, okHi;
                int lo = parts[0].toInt(&okLo);
                int hi = parts[1].toInt(&okHi);
                if (!okLo || !okHi || hi <= lo) continue;
                loadedWidths.append(hi - lo);
                loadedLo.append(lo);
                loadedHi.append(hi);
            } else {
                bool ok;
                int w = s.toInt(&ok);
                if (!ok || w <= 0) continue;
                loadedWidths.append(w);
                loadedLo.append(INT_MIN);
                loadedHi.append(INT_MIN);
            }
        }
        if (loadedWidths != m_filterWidths
                || loadedLo != m_filterCustomLo
                || loadedHi != m_filterCustomHi) {
            m_filterWidths = loadedWidths;
            m_filterCustomLo = loadedLo;
            m_filterCustomHi = loadedHi;
            rebuildFilterButtons();
        }
    }

    const int width = m_slice->filterHigh() - m_slice->filterLow();
    int bestIdx = -1, bestDist = INT_MAX;
    for (int i = 0; i < m_filterWidths.size(); ++i) {
        int dist = std::abs(width - m_filterWidths[i]);
        if (dist < bestDist) { bestDist = dist; bestIdx = i; }
    }
    if (bestIdx >= 0 && bestDist > m_filterWidths[bestIdx] / 10)
        bestIdx = -1;
    for (int i = 0; i < m_filterBtns.size(); ++i) {
        QSignalBlocker sb(m_filterBtns[i]);
        m_filterBtns[i]->setChecked(i == bestIdx);
    }
}

void VfoWidget::applyFilterPreset(int widthHz)
{
    if (!m_slice) return;
    int lo, hi;
    const QString& mode = m_slice->mode();

    if (mode == "DIGU") {
        // For widths < 3000 Hz, center the filter on the stored digu_offset.
        // SmartSDR behavior (fw v1.4.0.0): offset is the audio center frequency;
        // filter spans [offset - width/2, offset + width/2], clamped so lo >= 95.
        // For widths >= 3000 Hz, SmartSDR ignores the offset and runs from 95 Hz
        // upward — preserve that behavior unchanged.
        if (widthHz < 3000) {
            int offset = m_slice->diguOffset();
            lo = offset - widthHz / 2;
            hi = offset + widthHz / 2;
            if (lo < 95) {
                // Clamp: don't let lo drop below 95 Hz (carrier rejection)
                hi += (95 - lo);
                lo = 95;
            }
        } else {
            lo = 95;
            hi = widthHz;
        }
    } else if (mode == "DIGL") {
        // Mirror of DIGU: offset is negative (below carrier). For widths < 3000 Hz,
        // center on -digl_offset, clamped so hi <= -95.
        // For widths >= 3000 Hz, run from -95 downward.
        if (widthHz < 3000) {
            int offset = m_slice->diglOffset();
            hi = -offset + widthHz / 2;
            lo = -offset - widthHz / 2;
            if (hi > -95) {
                lo -= (hi + 95);
                hi = -95;
            }
        } else {
            lo = -widthHz;
            hi = -95;
        }
    } else if (mode == "LSB") {
        lo = -widthHz; hi = -95;
    } else if (mode == "RTTY") {
        // RTTY: RF_frequency = mark. Filter is relative to mark.
        // Space is at -rttyShift. Passband should encompass both tones.
        // Expand symmetrically around the midpoint between mark(0) and space(-shift).
        int shift = m_slice->rttyShift();
        int mid = -shift / 2;
        lo = mid - widthHz / 2;
        hi = mid + widthHz / 2;
    } else if (mode == "CW" || mode == "CWL") {
        // Centered on carrier — radio's BFO handles pitch offset
        lo = -widthHz / 2;
        hi =  widthHz / 2;
    } else if (mode == "AM" || mode == "SAM" || mode == "DSB"
               || mode == "FM" || mode == "NFM" || mode == "DFM") {
        lo = -(widthHz / 2); hi = (widthHz / 2);
    } else {
        // USB/FDV/etc: low cut at 95 Hz to reject carrier/hum
        lo = 95; hi = widthHz;
    }
    m_slice->setFilterWidth(lo, hi);
}

void VfoWidget::saveFilterPresets()
{
    if (!m_slice) return;
    QStringList parts;
    for (int i = 0; i < m_filterWidths.size(); ++i) {
        if (m_filterCustomLo[i] != INT_MIN) {
            // Custom edges: emit "lo:hi" so asymmetric setups persist (#2259).
            parts.append(QString("%1:%2").arg(m_filterCustomLo[i]).arg(m_filterCustomHi[i]));
        } else {
            parts.append(QString::number(m_filterWidths[i]));
        }
    }
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("FilterPresets_%1").arg(m_slice->mode()),
              parts.join(','));
    s.save();
}

void VfoWidget::setAntennaList(const QStringList& ants)
{
    m_antList = ants;
}

void VfoWidget::setTransmitModel(TransmitModel* txModel)
{
    m_txModel = txModel;
}

bool VfoWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_freqEdit
        && (event->type() == QEvent::ShortcutOverride
            || event->type() == QEvent::KeyPress)) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if ((ke->key() == Qt::Key_Escape || ke->key() == Qt::Key_Cancel)
            && cancelDirectEntry()) {
            event->accept();
            return true;
        }
    }

    // Double-click on DIG offset label → switch to inline edit
    if (obj == m_digOffsetLabel && event->type() == QEvent::MouseButtonDblClick) {
        if (m_digOffsetStack && m_digOffsetEdit) {
            int cur = m_slice ? ((m_slice->mode() == "DIGL")
                ? m_slice->diglOffset() : m_slice->diguOffset()) : 0;
            m_digOffsetEdit->setText(QString::number(cur));
            m_digOffsetStack->setCurrentIndex(1);
            m_digOffsetEdit->selectAll();
            m_digOffsetEdit->setFocus();
        }
        return true;
    }

    // Double-click on frequency label → open inline edit
    if (obj == m_freqLabel && event->type() == QEvent::MouseButtonDblClick) {
        beginDirectEntry();
        return true;
    }
    // Right-click on frequency label → context menu
    if (obj == m_freqLabel && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::RightButton && m_slice) {
            QMenu menu(this);
            menu.setStyleSheet(
                "QMenu { background: #1a1a2e; color: #c8d8e8; border: 1px solid #304060; }"
                "QMenu::item:selected { background: #00b4d8; color: #0f0f1a; }");
            menu.addAction("Add Spot", this, [this] {
                emit addSpotRequested(m_slice->frequency());
            });
            menu.exec(me->globalPosition().toPoint());
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto* lbl = qobject_cast<QLabel*>(obj);
        if (lbl) {
            int idx = m_tabBtns.indexOf(lbl);
            if (idx >= 0) {
                auto* me = static_cast<QMouseEvent*>(event);
                // Right-click on speaker tab (idx 0) toggles mute directly
                if (idx == 0 && me->button() == Qt::RightButton && m_slice) {
                    m_slice->setAudioMute(!m_slice->audioMute());
                    return true;
                }
                if (me->button() == Qt::LeftButton) {
                    showTab(idx);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ── RADE status indicator ─────────────────────────────────────────────────

#ifdef HAVE_RADE
void VfoWidget::setRadeActive(bool on)
{
    m_radeActive = on;
    if (m_radeStatusLabel) {
        m_radeStatusLabel->setVisible(on);
        if (!on) m_radeStatusLabel->setText("");
    }
}

void VfoWidget::setRadeSynced(bool synced)
{
    if (!m_radeActive || !m_radeStatusLabel) return;
    if (synced)
        m_radeStatusLabel->setText("RADE <font color='#00ff88'>\u25CF</font> ---");
    else
        m_radeStatusLabel->setText("RADE <font color='#505050'>\u25CB</font> ---");
}

void VfoWidget::setRadeSnr(float snrDb)
{
    if (!m_radeActive || !m_radeStatusLabel) return;
    QString color = (snrDb < 5.0f) ? "#e0e040" : "#00ff88";
    m_radeStatusLabel->setText(
        QString("RADE <font color='%1'>\u25CF</font> %2dB")
            .arg(color)
            .arg(static_cast<int>(snrDb)));
}

void VfoWidget::setRadeFreqOffset(float hz)
{
    if (!m_radeActive || !m_radeStatusLabel) return;
    // Append freq offset to existing text
    QString current = m_radeStatusLabel->text();
    int dbPos = current.indexOf("dB");
    if (dbPos > 0) {
        QString base = current.left(dbPos + 2);
        QString sign = (hz >= 0) ? "+" : "";
        m_radeStatusLabel->setText(
            QString("%1 %2%3Hz").arg(base, sign).arg(static_cast<int>(hz)));
    }
}
#endif

} // namespace AetherSDR
