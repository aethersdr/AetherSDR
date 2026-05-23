#include "RxApplet.h"
#include "FilterPassbandWidget.h"
#include "FrequencyEntryParser.h"
#include "GuardedSlider.h"
#include "ComboStyle.h"
#include "SliceColorManager.h"
#include "SliceLabel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMenu>
#include <QToolButton>
#include <QButtonGroup>
#include <QSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include "core/AppSettings.h"
#include <QAction>
#include <QFontMetrics>
#include <QPainter>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QStyle>
#include <QDoubleSpinBox>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <limits>

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
        p.setBrush(isDown() ? QColor(0, 0, 0) : QColor(0xc8, 0xd8, 0xe8));
        p.setPen(Qt::NoPen);
        const int cx = width() / 2, cy = height() / 2;
        QPolygon tri;
        if (m_dir == Left)
            tri << QPoint(cx - 5, cy) << QPoint(cx + 4, cy - 5) << QPoint(cx + 4, cy + 5);
        else
            tri << QPoint(cx + 5, cy) << QPoint(cx - 4, cy - 5) << QPoint(cx - 4, cy + 5);
        p.drawPolygon(tri);
    }
private:
    Dir m_dir;
};

namespace AetherSDR {

// ─── Helpers ──────────────────────────────────────────────────────────────────

static QString percentText(int value)
{
    return QStringLiteral("%1%").arg(value);
}

static QString panText(int value)
{
    if (value == 50)
        return QStringLiteral("C");
    return QStringLiteral("%1%2").arg(value < 50 ? QStringLiteral("L") : QStringLiteral("R"))
                                .arg(std::abs(value - 50));
}

// ── Style constants (matching docs/style/applet-style-guide.md) ───────────────

static constexpr const char* kButtonBase =
    "QPushButton { background: #1a2a3a; border: 1px solid #205070; "
    "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; "
    "padding: 1px 2px; }"
    "QPushButton:hover { background: #204060; }";

static constexpr const char* kSliderStyle =
    "QSlider::groove:horizontal { height: 4px; background: #203040; border-radius: 2px; }"
    "QSlider::handle:horizontal { width: 10px; height: 10px; margin: -3px 0;"
    "background: #00b4d8; border-radius: 5px; }";

static constexpr const char* kDimLabelStyle =
    "QLabel { color: #8090a0; font-size: 11px; }";

static constexpr const char* kInsetValueStyle =
    "QLabel { font-size: 10px; background: #0a0a18; border: 1px solid #1e2e3e; "
    "border-radius: 3px; padding: 1px 2px; color: #c8d8e8; }";

static const QString kBlueActive =
    "QPushButton:checked { background-color: #0070c0; color: #ffffff; "
    "border: 1px solid #0090e0; }";

static const QString kGreenActive =
    "QPushButton:checked { background-color: #006040; color: #00ff88; "
    "border: 1px solid #00a060; }";

static const QString kDisabledBtn =
    "QPushButton:disabled { background-color: #1a1a2a; color: #556070; "
    "border: 1px solid #2a3040; }";

static const QString kAmberActive =
    "QPushButton:checked { background-color: #604000; color: #ffb800; "
    "border: 1px solid #906000; }";

static constexpr int kLockedFrequencyFeedbackMs = 500;

static bool likelyTxAntennaFallbackToken(const QString& token)
{
    const QString upper = token.toUpper();
    if (upper.startsWith(QStringLiteral("RX")))
        return false;
    return upper.startsWith(QStringLiteral("ANT"))
        || upper.startsWith(QStringLiteral("TX"))
        || upper == QStringLiteral("XVTR");
}


// ── Per-mode filter widths and step sizes (from SmartSDR) ─────────────────────

struct ModeSettings {
    QVector<int> filterWidths;   // Hz — empty means no filter presets (FM modes)
    QVector<int> stepSizes;      // Hz
};

static const ModeSettings& modeSettingsFor(const QString& mode)
{
    // USB / LSB (default) — first 6 of VfoWidget's 8 presets
    static const ModeSettings ssbSettings{
        {1800, 2100, 2400, 2700, 2900, 3300},
        {1, 10, 50, 100, 500, 1000, 2000, 3000}
    };
    // AM / SAM — double-sideband: width split ±half around carrier
    static const ModeSettings amSettings{
        {5600, 6000, 8000, 10000, 12000, 14000},
        {250, 500, 2500, 3000, 5000, 9000, 10000}
    };
    // CW
    static const ModeSettings cwSettings{
        {50, 100, 250, 400},
        {1, 5, 10, 50, 100, 200, 400}
    };
    // DIGL / DIGU
    static const ModeSettings digSettings{
        {100, 300, 600, 1000, 1500, 2000},
        {1, 5, 10, 20, 100, 250, 500, 1000}
    };
    // RTTY
    static const ModeSettings rttySettings{
        {250, 300, 350, 400, 500, 1000},
        {1, 5, 10, 20, 100, 250, 500, 1000}
    };
    // FM / NFM / DFM — no filter presets
    static const ModeSettings fmSettings{
        {},
        {50, 250, 500, 2500, 3000, 5000, 10000, 12500}
    };

    if (mode == "USB" || mode == "LSB")  return ssbSettings;
    if (mode == "AM"  || mode == "SAM")  return amSettings;
    if (mode == "CW")                    return cwSettings;
    if (mode == "DIGU" || mode == "DIGL" || mode == "NT") return digSettings;
    if (mode == "RTTY")                  return rttySettings;
    if (mode == "FM" || mode == "NFM" || mode == "DFM") return fmSettings;
    if (mode.startsWith("FDV"))          return digSettings;  // FreeDV digital voice
    return ssbSettings;  // fallback for unknown modes
}

// ── Standard CTCSS tone table (EIA/TIA-603) ──────────────────────────────────

struct CTCSSTone {
    int code;
    const char* designation;
    double frequency;
};

static constexpr CTCSSTone CTCSS_TONES[] = {
    { 1, "XZ", 67.0},  { 2, "XA", 71.9},  { 3, "WA", 74.4},  { 4, "XB", 77.0},
    { 5, "WB", 79.7},  { 6, "YZ", 82.5},  { 7, "YA", 85.4},  { 8, "YB", 88.5},
    { 9, "ZZ", 91.5},  {10, "ZA", 94.8},  {11, "ZB", 97.4},  {12, "1Z",100.0},
    {13, "1A",103.5},  {14, "1B",107.2},  {15, "2Z",110.9},  {16, "2A",114.8},
    {17, "2B",118.8},  {18, "3Z",123.0},  {19, "3A",127.3},  {20, "3B",131.8},
    {21, "4Z",136.5},  {22, "4A",141.3},  {23, "4B",146.2},  {24, "5Z",151.4},
    {25, "5A",156.7},  {26, "5B",162.2},  {27, "6Z",167.9},  {28, "6A",173.8},
    {29, "6B",179.9},  {30, "7Z",186.2},  {31, "7A",192.8},  {32, "M1",203.5},
    {33, "8Z",206.5},  {34, "M2",210.7},  {35, "M3",218.1},  {36, "M4",225.7},
    {37, "9Z",229.1},  {38, "M5",233.6},  {39, "M6",241.8},  {40, "M7",250.3},
    {41, "0Z",254.1},
};
static constexpr int CTCSS_COUNT = sizeof(CTCSS_TONES) / sizeof(CTCSS_TONES[0]);

// Small checkable button used throughout the applet.
static QPushButton* mkToggle(const QString& text, QWidget* parent = nullptr)
{
    auto* b = new QPushButton(text, parent);
    b->setCheckable(true);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setFixedHeight(20);
    b->setStyleSheet(kButtonBase);
    return b;
}

static QPushButton* mkLeft(QWidget* parent = nullptr)  { return new TriBtn(TriBtn::Left,  parent); }
static QPushButton* mkRight(QWidget* parent = nullptr) { return new TriBtn(TriBtn::Right, parent); }

// ─── Construction ─────────────────────────────────────────────────────────────

RxApplet::RxApplet(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    // Recall the last user-chosen Manual squelch threshold from prior
    // sessions.  Auto mode clobbers the slice's squelchLevel with
    // algorithm-suggested values, so the radio can't be relied on to
    // preserve the operator's manual preference across mode cycles or
    // launches — we persist it client-side.
    m_sqlManualLevel = std::clamp(
        AppSettings::instance().value("LastManualSquelchLevel", "20").toInt(),
        0, 100);
    m_lockedFrequencyTimer.setSingleShot(true);
    connect(&m_lockedFrequencyTimer, &QTimer::timeout,
            this, &RxApplet::clearLockedFrequencyFeedback);
    buildUI();
}

void RxApplet::buildUI()
{
    // Outer layout: title bar flush to edges, then padded content below.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* inner = new QWidget;
    auto* root = new QVBoxLayout(inner);
    root->setContentsMargins(4, 2, 4, 2);
    root->setSpacing(2);
    outer->addWidget(inner);

    // ── Slice tab toggle row (populated later by setMaxSlices) ──────────
    {
        m_sliceTabRow = new QWidget;
        m_sliceTabRow->setVisible(false);  // hidden until setMaxSlices called
        auto* tabLayout = new QHBoxLayout(m_sliceTabRow);
        tabLayout->setContentsMargins(0, 0, 0, 0);
        tabLayout->setSpacing(2);
        m_sliceGroup = new QButtonGroup(this);
        m_sliceGroup->setExclusive(true);
        tabLayout->addStretch();

        m_muteAllBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x87"));  // 🔇
        m_muteAllBtn->setToolTip("Mute all slices (click again to unmute all)");
        m_muteAllBtn->setFixedSize(28, 20);
        m_muteAllBtn->setStyleSheet(
            "QPushButton { background: #2a2a2a; border: 1px solid #504040; "
            "border-radius: 3px; font-size: 12px; padding: 0; }"
            "QPushButton:hover { background: #3a3030; border-color: #a06060; }"
            "QPushButton:pressed { background: #6a2020; }");
        connect(m_muteAllBtn, &QPushButton::clicked,
                this, &RxApplet::muteAllToggled);
        tabLayout->addWidget(m_muteAllBtn);

        root->addWidget(m_sliceTabRow);
    }

    // ── Header: slice badge | lock | RX ant | TX ant | filter width | QSK ──
    {
        auto* row = new QHBoxLayout;
        m_headerRow = row;
        row->setSpacing(3);

        // Slice letter badge (A/B/C/D)
        m_sliceBadge = new QLabel("A");
        m_sliceBadge->setFixedSize(20, 20);
        m_sliceBadge->setAlignment(Qt::AlignCenter);
        m_sliceBadge->setTextFormat(Qt::RichText);  // slice letter may be HTML (#2606)
        m_sliceBadge->setStyleSheet(
            "QLabel { background: #0070c0; color: #ffffff; "
            "border-radius: 3px; font-weight: bold; font-size: 11px; }");
        row->addWidget(m_sliceBadge);

        // Tune-lock toggle (🔓 unlocked / 🔒 locked)
        m_lockBtn = new QPushButton("\U0001F513");  // 🔓
        m_lockBtn->setCheckable(true);
        m_lockBtn->setFixedSize(20, 20);
        m_lockBtn->setFlat(true);
        m_lockBtn->setStyleSheet(
            "QPushButton { font-size: 13px; padding: 0; }"
            "QPushButton:checked { color: #4488ff; }");
        connect(m_lockBtn, &QPushButton::toggled, this, [this](bool locked) {
            m_lockBtn->setText(locked ? "\U0001F512" : "\U0001F513");
            if (m_slice) m_slice->setLocked(locked);
        });
        row->addWidget(m_lockBtn);

        // RX antenna dropdown (blue text, no border)
        m_rxAntBtn = new QPushButton("ANT1");
        m_rxAntBtn->setFlat(true);
        m_rxAntBtn->setStyleSheet(
            "QPushButton { color: #4488ff; background: transparent; border: none; "
            "font-size: 10px; font-weight: bold; padding: 0 2px; }"
            "QPushButton:hover { color: #66aaff; }");
        connect(m_rxAntBtn, &QPushButton::clicked, this, [this] {
            QMenu menu(this);
            const QString cur = m_slice ? m_slice->rxAntenna() : "";
            const QStringList options = m_slice && !m_slice->rxAntennaList().isEmpty()
                ? m_slice->rxAntennaList()
                : m_antList;
            for (const QString& ant : options) {
                QAction* act = menu.addAction(antennaMenuLabel(ant, options));
                act->setData(ant);
                act->setCheckable(true);
                act->setChecked(ant == cur);
                act->setToolTip(ant);
                act->setStatusTip(ant);
            }
            const QAction* sel = menu.exec(
                m_rxAntBtn->mapToGlobal(QPoint(0, m_rxAntBtn->height())));
            if (sel && m_slice)
                m_slice->setRxAntenna(sel->data().toString());
        });
        row->addWidget(m_rxAntBtn);

        // TX antenna dropdown (red text, no border)
        m_txAntBtn = new QPushButton("ANT1");
        m_txAntBtn->setFlat(true);
        m_txAntBtn->setStyleSheet(
            "QPushButton { color: #ff4444; background: transparent; border: none; "
            "font-size: 10px; font-weight: bold; padding: 0 2px; }"
            "QPushButton:hover { color: #ff6666; }");
        connect(m_txAntBtn, &QPushButton::clicked, this, [this] {
            QMenu menu(this);
            const QString cur = m_slice ? m_slice->txAntenna() : "";
            const QStringList options = txAntennaOptions();
            for (const QString& ant : options) {
                QAction* act = menu.addAction(antennaMenuLabel(ant, options));
                act->setData(ant);
                act->setCheckable(true);
                act->setChecked(ant == cur);
                act->setToolTip(ant);
                act->setStatusTip(ant);
            }
            const QAction* sel = menu.exec(
                m_txAntBtn->mapToGlobal(QPoint(0, m_txAntBtn->height())));
            if (sel && m_slice)
                m_slice->setTxAntenna(sel->data().toString());
        });
        row->addWidget(m_txAntBtn);

        row->addStretch(1);

        // Filter width label (e.g. "2.7K") — centered between ANT and QSK
        m_filterWidthLbl = new QLabel("2.7K");
        m_filterWidthLbl->setAlignment(Qt::AlignCenter);
        m_filterWidthLbl->setStyleSheet(
            "QLabel { color: #00c8ff; font-size: 11px; font-weight: bold; }");
        row->addWidget(m_filterWidthLbl);

        row->addStretch(1);

        // QSK indicator (read-only — controlled via CW applet Breakin button)
        m_qskBtn = new QPushButton("QSK");
        m_qskBtn->setCheckable(true);
        m_qskBtn->setFlat(true);
        m_qskBtn->setEnabled(false);
        m_qskBtn->setStyleSheet(
            "QPushButton { color: #8090a0; background: transparent; border: none; "
            "font-size: 10px; font-weight: bold; padding: 0 2px; }"
            "QPushButton:checked { color: #ffb800; }");
        row->addWidget(m_qskBtn);

        root->addLayout(row);
    }

    // ── Frequency row ──────────────────────────────────────────────────────
    {
        m_freqRow = new QHBoxLayout;
        m_freqRow->setContentsMargins(0, 0, 0, 0);
        m_freqRow->setSpacing(0);

        // TX slice indicator badge — click to set this slice as TX
        m_txBadge = new QPushButton("TX");
        m_txBadge->setFixedSize(20, 20);
        m_txBadge->setStyleSheet(
            "QPushButton { background: #405060; color: #ffffff; "
            "border-radius: 3px; border: none; font-weight: bold; font-size: 10px;"
            " padding: 0px; margin: 0px; }"
            "QPushButton:hover { background: #506070; }");
        connect(m_txBadge, &QPushButton::clicked, this, [this] {
            if (m_slice) m_slice->setTxSlice(!m_slice->isTxSlice());
        });
        m_freqRow->addWidget(m_txBadge);
        m_freqRow->addSpacing(4);

        // Mode selector combo
        m_modeCombo = new GuardedComboBox;
        m_modeCombo->setFixedHeight(20);
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
            // "RADE" is client-side only — the radio echoes back the real mode
            // (DIGL/DIGU) immediately, so mode() == "RADE" is never true in
            // steady state. Emit unconditionally; MainWindow's
            // sliceId == m_radeSliceId check is the authoritative filter that
            // prevents spurious deactivations from non-RADE slices (#2026).
            emit radeActivated(false, m_slice ? m_slice->sliceId() : -1);
#endif
            if (m_slice) m_slice->setMode(mode);
        });
        m_freqRow->addWidget(m_modeCombo);

        m_freqStack = new QStackedWidget;
        m_freqStack->setFixedHeight(34);

        m_freqLabel = new QLabel("0.000.000");
        m_freqLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_freqLabel->setStyleSheet(
            "QLabel { color: #c8d8e8; font-size: 28px; font-weight: bold;"
            " background: transparent; padding: 0; margin: 0; }");
        m_freqLabel->installEventFilter(this);
        m_freqStack->addWidget(m_freqLabel);

        m_freqEdit = new QLineEdit;
        m_freqEdit->setStyleSheet(
            "QLineEdit { background: #0a0a18; border: 1px solid #00b4d8;"
            " border-radius: 3px; color: #00e5ff; font-size: 20px;"
            " font-weight: bold; padding: 0 4px; }");
        m_freqEdit->setAlignment(Qt::AlignRight);
        m_freqEdit->setPlaceholderText("MHz");
        m_freqEdit->installEventFilter(this);
        m_freqStack->addWidget(m_freqEdit);
        m_freqStack->setCurrentIndex(0);

        connect(m_freqEdit, &QLineEdit::returnPressed, this, [this] {
            const QString text = m_freqEdit->text().trimmed();
            if (!text.isEmpty() && m_slice) {
                QString clean = FrequencyEntryParser::normalizedMhzText(text);
                bool ok = false;
                double freqMhz = clean.toDouble(&ok);
                const bool explicitMhzEntry = FrequencyEntryParser::isExplicitMhzEntry(text, clean);
                const bool onXvtr = m_slice &&
                    (m_slice->rxAntenna().startsWith("XVT") || m_slice->frequency() > 54.0);
                const bool highExplicitMhzEntry = ok && explicitMhzEntry && freqMhz > 54.0;
                const double maxMhz = (onXvtr || highExplicitMhzEntry) ? 50000.0 : 54.0;
                if (onXvtr) {
                    // 3-digit-band convenience (2m/70cm): 1446 → 144.6.
                    // Skip for 23cm/microwave — 1296 means 1296 MHz.
                    const double sliceMhz = m_slice->frequency();
                    const bool threeDigitBand = sliceMhz >= 100.0 && sliceMhz < 1000.0;
                    if (ok && threeDigitBand && freqMhz > 450.0 && !clean.contains('.')) {
                        int digits = clean.length();
                        if (digits >= 4) {
                            clean.insert(3, '.');
                            freqMhz = clean.toDouble(&ok);
                        }
                    }
                } else if (!highExplicitMhzEntry) {
                    if (ok && freqMhz > 54000.0) freqMhz /= 1e6;
                    else if (ok && freqMhz > 54.0) freqMhz /= 1e3;
                }
                if (ok && freqMhz >= 0.001 && freqMhz <= maxMhz)
                    emit directEntryCommitted(freqMhz, QStringLiteral("rx-direct-entry"));
            }
            m_freqStack->setCurrentIndex(0);
        });
        connect(m_freqEdit, &QLineEdit::editingFinished, this, [this] {
            m_freqStack->setCurrentIndex(0);
        });

        m_freqRow->addStretch(1);
        m_freqRow->addWidget(m_freqStack);
        root->addLayout(m_freqRow);
    }

    // ── Two-column area ─────────────────────────────────────────────────────
    auto* columns = new QHBoxLayout;
    columns->setSpacing(4);

    // ── Left column (60%) ────────────────────────────────────────────────
    auto* leftCol = new QVBoxLayout;
    leftCol->setSpacing(2);

    // Step size
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(0);
        auto* lbl = new QLabel("STEP:");
        lbl->setStyleSheet(kDimLabelStyle);
        lbl->setFixedWidth(34);
        row->addWidget(lbl);

        m_stepDown  = mkLeft();
        m_stepLabel = new ScrollableLabel(formatStepLabel(m_stepSizes[m_stepIdx]));
        m_stepLabel->setAlignment(Qt::AlignCenter);
        m_stepLabel->setStyleSheet(
            "QLabel { font-size: 11px; background: #0a0a18; border: 1px solid #1e2e3e; "
            "border-radius: 3px; padding: 1px 2px; }");
        m_stepUp = mkRight();

        auto stepDown = [this] {
            if (m_stepIdx > 0) {
                m_stepIdx--;
                m_stepLabel->setText(formatStepLabel(m_stepSizes[m_stepIdx]));
                emit stepSizeChanged(m_stepSizes[m_stepIdx]);
            }
        };
        auto stepUp = [this] {
            if (m_stepIdx < m_stepSizes.size() - 1) {
                m_stepIdx++;
                m_stepLabel->setText(formatStepLabel(m_stepSizes[m_stepIdx]));
                emit stepSizeChanged(m_stepSizes[m_stepIdx]);
            }
        };
        connect(m_stepDown, &QPushButton::clicked, this, stepDown);
        connect(m_stepUp, &QPushButton::clicked, this, stepUp);
        connect(m_stepLabel, &ScrollableLabel::scrolled, this, [stepUp, stepDown](int dir) {
            if (dir > 0) stepUp(); else stepDown();
        });

        row->addWidget(m_stepDown);
        row->addWidget(m_stepLabel, 1);
        row->addWidget(m_stepUp);
        leftCol->addLayout(row);
    }

    // Filter presets (dynamically rebuilt on mode change)
    {
        m_filterContainer = new QWidget;
        m_filterGrid = new QGridLayout(m_filterContainer);
        m_filterGrid->setContentsMargins(0, 0, 0, 0);
        m_filterGrid->setSpacing(2);
        rebuildFilterButtons();
        leftCol->addWidget(m_filterContainer);
    }

    // Visual filter passband widget (draggable lo/hi edges)
    {
        m_filterPassband = new AetherSDR::FilterPassbandWidget;
        m_filterPassband->setMinimumHeight(40);
        m_filterPassband->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(m_filterPassband, &AetherSDR::FilterPassbandWidget::filterChanged,
                this, [this](int lo, int hi) {
            if (m_slice) m_slice->setFilterWidth(lo, hi);
        });
        leftCol->addWidget(m_filterPassband);
    }

    // FM duplex/repeater controls (hidden by default, shown for FM modes)
    {
        m_fmContainer = new QWidget;
        m_fmContainer->setVisible(false);
        auto* fmLayout = new QVBoxLayout(m_fmContainer);
        fmLayout->setContentsMargins(0, 0, 0, 0);
        fmLayout->setSpacing(2);

        // Tone mode dropdown
        {
            auto* row = new QHBoxLayout;
            row->setSpacing(4);
            m_toneModeCmb = new GuardedComboBox;
            m_toneModeCmb->addItem("Off",      QString("off"));
            m_toneModeCmb->addItem("CTCSS TX", QString("ctcss_tx"));
            AetherSDR::applyComboStyle(m_toneModeCmb);
            row->addWidget(m_toneModeCmb, 1);
            fmLayout->addLayout(row);

            connect(m_toneModeCmb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this](int idx) {
                if (m_toneModeCmb->signalsBlocked()) return;
                const QString mode = m_toneModeCmb->itemData(idx).toString();
                if (m_slice) m_slice->setFmToneMode(mode);
                m_toneValueCmb->setEnabled(mode == "ctcss_tx");
            });
        }

        // CTCSS tone value dropdown
        {
            m_toneValueCmb = new GuardedComboBox;
            for (int i = 0; i < CTCSS_COUNT; ++i) {
                const auto& t = CTCSS_TONES[i];
                m_toneValueCmb->addItem(
                    QString("%1 %2 %3").arg(t.code).arg(t.designation)
                        .arg(t.frequency, 0, 'f', 1),
                    QString::number(t.frequency, 'f', 1));
            }
            AetherSDR::applyComboStyle(m_toneValueCmb);
            m_toneValueCmb->setEnabled(false);  // enabled only when CTCSS TX
            fmLayout->addWidget(m_toneValueCmb);

            connect(m_toneValueCmb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this](int idx) {
                if (m_toneValueCmb->signalsBlocked()) return;
                if (m_slice)
                    m_slice->setFmToneValue(m_toneValueCmb->itemData(idx).toString());
            });
        }

        // Offset frequency
        {
            auto* row = new QHBoxLayout;
            row->setSpacing(4);
            auto* lbl = new QLabel("Offset:");
            lbl->setStyleSheet(kDimLabelStyle);
            row->addWidget(lbl);

            m_offsetSpin = new QDoubleSpinBox;
            m_offsetSpin->setRange(0.0, 100.0);
            m_offsetSpin->setDecimals(3);
            m_offsetSpin->setSingleStep(0.1);
            m_offsetSpin->setValue(0.0);
            m_offsetSpin->setSuffix(" Mhz");
            m_offsetSpin->setStyleSheet(
                "QDoubleSpinBox { background: #0a0a18; border: 1px solid #1e2e3e; "
                "border-radius: 3px; color: #c8d8e8; font-size: 10px; padding: 1px 2px; }"
                "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 0; }");
            row->addWidget(m_offsetSpin, 1);
            fmLayout->addLayout(row);

            connect(m_offsetSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                    this, [this](double val) {
                if (m_offsetSpin->signalsBlocked()) return;
                if (m_slice) {
                    m_slice->setFmRepeaterOffsetFreq(val);
                    // Recompute tx_offset_freq based on current direction
                    applyOffsetDir(m_slice->repeaterOffsetDir());
                }
            });
        }

        // Offset direction: − | Simplex | + | REV
        {
            auto* row = new QHBoxLayout;
            row->setSpacing(2);

            m_offsetDown = mkToggle(QString::fromUtf8("\xe2\x88\x92")); // −
            m_offsetDown->setStyleSheet(QString(kButtonBase) + kBlueActive);
            connect(m_offsetDown, &QPushButton::clicked, this, [this] {
                applyOffsetDir("down");
            });
            row->addWidget(m_offsetDown);

            m_simplexBtn = mkToggle("Simplex");
            m_simplexBtn->setStyleSheet(QString(kButtonBase) + kBlueActive);
            m_simplexBtn->setChecked(true);
            connect(m_simplexBtn, &QPushButton::clicked, this, [this] {
                applyOffsetDir("simplex");
            });
            row->addWidget(m_simplexBtn);

            m_offsetUp = mkToggle("+");
            m_offsetUp->setStyleSheet(QString(kButtonBase) + kBlueActive);
            connect(m_offsetUp, &QPushButton::clicked, this, [this] {
                applyOffsetDir("up");
            });
            row->addWidget(m_offsetUp);

            m_revBtn = mkToggle("REV");
            m_revBtn->setStyleSheet(QString(kButtonBase) + kAmberActive);
            connect(m_revBtn, &QPushButton::toggled, this, [this](bool on) {
                if (m_revBtn->signalsBlocked()) return;
                if (!m_slice) return;
                // REV flips the sign of tx_offset_freq
                double offset = m_slice->fmRepeaterOffsetFreq();
                const QString& dir = m_slice->repeaterOffsetDir();
                if (dir == "up")
                    m_slice->setTxOffsetFreq(on ? -offset : offset);
                else if (dir == "down")
                    m_slice->setTxOffsetFreq(on ? offset : -offset);
            });
            row->addWidget(m_revBtn);

            fmLayout->addLayout(row);
        }

        leftCol->addWidget(m_fmContainer);
    }

    // DSP toggles removed — use VFO DSP tab or spectrum overlay instead

    columns->addLayout(leftCol, 2);  // 40%

    // ── Right column (40%) ───────────────────────────────────────────────
    auto* rightCol = new QVBoxLayout;
    rightCol->setSpacing(2);

    // AF gain
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_muteBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x8A")); // 🔊
        m_muteBtn->setCheckable(true);
        m_muteBtn->setFixedSize(18, 18);
        m_muteBtn->setStyleSheet(
            "QPushButton { background: transparent; border: none; font-size: 12px; padding: 0px; }"
            "QPushButton:hover { background: #204060; border-radius: 3px; }");
        connect(m_muteBtn, &QPushButton::toggled, this, [this](bool muted) {
            m_muteBtn->setText(muted
                ? QString::fromUtf8("\xF0\x9F\x94\x87")    // 🔇
                : QString::fromUtf8("\xF0\x9F\x94\x8A"));  // 🔊
            if (m_slice) m_slice->setAudioMute(muted);
        });
        row->addWidget(m_muteBtn);

        m_afSlider = new GuardedSlider(Qt::Horizontal);
        m_afSlider->setRange(0, 100);
        m_afSlider->setValue(70);
        static_cast<GuardedSlider*>(m_afSlider)->setDragValueFormatter(percentText);
        m_afSlider->setStyleSheet(kSliderStyle);
        row->addWidget(m_afSlider, 1);

        connect(m_afSlider, &QSlider::valueChanged, this, [this](int v) {
            if (m_slice) m_slice->setAudioGain(v);
            emit afGainChanged(v);
        });
        rightCol->addLayout(row);
    }

    // Audio pan (L ←→ R)
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        auto* lLbl = new QLabel("L");
        lLbl->setStyleSheet(kDimLabelStyle);
        row->addWidget(lLbl);

        m_panSlider = new CenterMarkSlider(50, Qt::Horizontal);
        m_panSlider->setRange(0, 100);
        m_panSlider->setValue(50);
        static_cast<GuardedSlider*>(m_panSlider)->setDragValueFormatter(panText);
        m_panSlider->setStyleSheet(kSliderStyle);
        row->addWidget(m_panSlider, 1);

        auto* rLbl = new QLabel("R");
        rLbl->setStyleSheet(kDimLabelStyle);
        row->addWidget(rLbl);

        connect(m_panSlider, &QSlider::valueChanged, this, [this](int v) {
            if (m_slice) m_slice->setAudioPan(v);
        });
        rightCol->addLayout(row);
    }

    // Squelch — single row with a 3-way cycle button + threshold slider.
    // Button cycles Off → Manual (SQL) → Auto (AUTO) → Off on each click.
    // Slider is enabled only in Manual mode; Auto drives the level itself.
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_sqlBtn = new QPushButton("SQL");
        m_sqlBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_sqlBtn->setFixedHeight(20);
        m_sqlBtn->setFixedWidth(52);
        row->addWidget(m_sqlBtn);

        m_sqlSlider = new GuardedSlider(Qt::Horizontal);
        m_sqlSlider->setRange(0, 100);
        m_sqlSlider->setValue(20);
        static_cast<GuardedSlider*>(m_sqlSlider)->setDragValueFormatter([this](int v) {
            if (m_sqlMode == SqlMode::Auto)
                return QStringLiteral("%1 dB").arg(v);
            return QString::number(v);
        });
        m_sqlSlider->setStyleSheet(kSliderStyle);
        row->addWidget(m_sqlSlider, 1);

        applySqlModeVisuals();
        connect(m_sqlBtn, &QPushButton::clicked,
                this, &RxApplet::cycleSqlMode);
        connect(m_sqlSlider, &QSlider::valueChanged, this, [this](int v) {
            if (m_sqlMode == SqlMode::Manual) {
                // Cache the user's chosen manual level so re-entering
                // Manual from Auto/Off restores it, and persist it so the
                // choice survives launch (the slice's squelchLevel gets
                // clobbered by Auto's algorithm pushes and can no longer
                // be trusted as the manual baseline).
                m_sqlManualLevel = v;
                auto& s = AppSettings::instance();
                s.setValue("LastManualSquelchLevel", QString::number(v));
                s.save();
                if (m_slice)
                    m_slice->setSquelch(true, v);
            } else if (m_sqlMode == SqlMode::Auto) {
                // Auto SQL: slider sets the dB margin above the measured
                // noise floor (5–20 dB).  Persist + broadcast so every
                // pan's auto-squelch algorithm picks up the new margin
                // and the suggested level recomputes on the next FFT.
                auto& s = AppSettings::instance();
                s.setValue("AutoSqlMarginDb", QString::number(v));
                s.save();
                emit autoSqlMarginDbChanged(v);
            }
        });
        rightCol->addLayout(row);
    }

    // AGC mode + threshold (wrapped in container for FM hide)
    {
        m_agcContainer = new QWidget;
        auto* agcRow = new QHBoxLayout(m_agcContainer);
        agcRow->setContentsMargins(0, 0, 0, 0);
        agcRow->setSpacing(4);

        m_agcCombo = new GuardedComboBox;
        m_agcCombo->addItem("Off",  QString("off"));
        m_agcCombo->addItem("Slow", QString("slow"));
        m_agcCombo->addItem("Med",  QString("med"));
        m_agcCombo->addItem("Fast", QString("fast"));
        m_agcCombo->setCurrentIndex(2);
        m_agcCombo->setFixedWidth(52);
        AetherSDR::applyComboStyle(m_agcCombo);
        connect(m_agcCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
            if (m_slice) m_slice->setAgcMode(m_agcCombo->itemData(idx).toString());
        });
        agcRow->addWidget(m_agcCombo);

        m_agcTSlider = new GuardedSlider(Qt::Horizontal);
        m_agcTSlider->setRange(0, 100);
        m_agcTSlider->setValue(65);
        m_agcTSlider->setStyleSheet(kSliderStyle);
        agcRow->addWidget(m_agcTSlider, 1);

        connect(m_agcTSlider, &QSlider::valueChanged, this, [this](int v) {
            if (m_slice) {
                if (m_slice->agcMode() == "off") {
                    m_agcTSlider->setToolTip(QString("AGC Off Level: %1").arg(v));
                    m_slice->setAgcOffLevel(v);
                } else {
                    m_agcTSlider->setToolTip(QString("AGC Threshold: %1").arg(v));
                    m_slice->setAgcThreshold(v);
                }
            }
        });
        rightCol->addWidget(m_agcContainer);
    }

    rightCol->addStretch(1);

    // RIT (wrapped in container for FM hide)
    {
        m_ritContainer = new QWidget;
        auto* row = new QHBoxLayout(m_ritContainer);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);

        m_ritOnBtn = mkToggle("RIT");
        m_ritOnBtn->setStyleSheet(QString(kButtonBase) + kAmberActive);
        row->addWidget(m_ritOnBtn);

        m_ritZero = new QPushButton("0");
        m_ritZero->setStyleSheet(
            QString(kButtonBase) + "QPushButton { padding: 1px 4px; }");
        connect(m_ritZero, &QPushButton::clicked, this, [this] {
            if (m_slice) m_slice->setRit(m_ritOnBtn->isChecked(), 0);
        });
        row->addSpacing(2);
        row->addWidget(m_ritZero);
        row->addSpacing(2);

        m_ritMinus = mkLeft();
        row->addWidget(m_ritMinus);

        m_ritLabel = new ScrollableLabel("+0 Hz");
        m_ritLabel->setAlignment(Qt::AlignCenter);
        m_ritLabel->setStyleSheet(
            "QLabel { font-size: 10px; background: #0a0a18; border: 1px solid #1e2e3e; "
            "border-radius: 3px; padding: 0px 2px; }");
        row->addWidget(m_ritLabel, 1);
        connect(m_ritLabel, &ScrollableLabel::scrolled, this, [this](int dir) {
            if (m_slice) m_slice->setRit(m_ritOnBtn->isChecked(),
                m_slice->ritFreq() + dir * RIT_STEP_HZ);
        });

        m_ritPlus = mkRight();
        row->addWidget(m_ritPlus);

        rightCol->addWidget(m_ritContainer);

        connect(m_ritOnBtn, &QPushButton::toggled, this, [this](bool on) {
            if (m_slice) m_slice->setRit(on, m_slice->ritFreq());
        });
        connect(m_ritMinus, &QPushButton::clicked, this, [this] {
            if (!m_slice) return;
            m_slice->setRit(m_ritOnBtn->isChecked(), m_slice->ritFreq() - RIT_STEP_HZ);
        });
        connect(m_ritPlus, &QPushButton::clicked, this, [this] {
            if (!m_slice) return;
            m_slice->setRit(m_ritOnBtn->isChecked(), m_slice->ritFreq() + RIT_STEP_HZ);
        });
    }

    // XIT (wrapped in container for FM hide)
    {
        m_xitContainer = new QWidget;
        auto* row = new QHBoxLayout(m_xitContainer);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);

        m_xitOnBtn = mkToggle("XIT");
        m_xitOnBtn->setStyleSheet(QString(kButtonBase) + kAmberActive);
        row->addWidget(m_xitOnBtn);

        m_xitZero = new QPushButton("0");
        m_xitZero->setStyleSheet(
            QString(kButtonBase) + "QPushButton { padding: 1px 4px; }");
        connect(m_xitZero, &QPushButton::clicked, this, [this] {
            if (m_slice) m_slice->setXit(m_xitOnBtn->isChecked(), 0);
        });
        row->addSpacing(2);
        row->addWidget(m_xitZero);
        row->addSpacing(2);

        m_xitMinus = mkLeft();
        row->addWidget(m_xitMinus);

        m_xitLabel = new ScrollableLabel("+0 Hz");
        m_xitLabel->setAlignment(Qt::AlignCenter);
        m_xitLabel->setStyleSheet(
            "QLabel { font-size: 10px; background: #0a0a18; border: 1px solid #1e2e3e; "
            "border-radius: 3px; padding: 0px 2px; }");
        row->addWidget(m_xitLabel, 1);
        connect(m_xitLabel, &ScrollableLabel::scrolled, this, [this](int dir) {
            if (m_slice) m_slice->setXit(m_xitOnBtn->isChecked(),
                m_slice->xitFreq() + dir * RIT_STEP_HZ);
        });

        m_xitPlus = mkRight();
        row->addWidget(m_xitPlus);

        rightCol->addWidget(m_xitContainer);

        connect(m_xitOnBtn, &QPushButton::toggled, this, [this](bool on) {
            if (m_slice) m_slice->setXit(on, m_slice->xitFreq());
        });
        connect(m_xitMinus, &QPushButton::clicked, this, [this] {
            if (!m_slice) return;
            m_slice->setXit(m_xitOnBtn->isChecked(), m_slice->xitFreq() - RIT_STEP_HZ);
        });
        connect(m_xitPlus, &QPushButton::clicked, this, [this] {
            if (!m_slice) return;
            m_slice->setXit(m_xitOnBtn->isChecked(), m_slice->xitFreq() + RIT_STEP_HZ);
        });
    }

    columns->addLayout(rightCol, 3);  // 60%

    root->addLayout(columns);

    // Tooltips
    m_lockBtn->setToolTip("Locks the VFO frequency to prevent accidental tuning.");
    m_rxAntBtn->setToolTip("Select the receive antenna port.");
    m_txAntBtn->setToolTip("Select the transmit antenna port.");
    m_stepDown->setToolTip("Decrease tuning step size.");
    m_stepLabel->setToolTip("Current tuning step size. Scroll to change.");
    m_stepUp->setToolTip("Increase tuning step size.");
    m_muteBtn->setToolTip("Mutes this slice's audio output.");
    m_afSlider->setToolTip("Audio output volume for this slice.");
    m_sqlBtn->setToolTip("Squelch gate \u2014 silences audio when the signal drops below the threshold.");
    m_sqlSlider->setToolTip("Squelch threshold. Increase to require a stronger signal before audio opens.");
    // Auto SQL tooltip set inline at construction
    m_agcCombo->setToolTip("AGC speed. Slow resists pumping on quiet bands; Fast tracks rapid signal changes.");
    m_agcTSlider->setToolTip(QString("AGC Threshold: %1").arg(m_agcTSlider->value()));
    m_ritOnBtn->setToolTip("Receive Incremental Tuning \u2014 offsets the receive frequency without moving transmit.");
    m_ritZero->setToolTip("Resets the RIT offset to zero.");
    m_xitOnBtn->setToolTip("Transmit Incremental Tuning \u2014 offsets the transmit frequency without moving receive.");
    m_xitZero->setToolTip("Resets the XIT offset to zero.");

    // Accessible names for VoiceOver / screen reader support (#870)
    m_sliceBadge->setAccessibleName("Slice letter");
    m_lockBtn->setAccessibleName("VFO lock");
    m_lockBtn->setAccessibleDescription("Lock VFO frequency to prevent accidental tuning");
    m_rxAntBtn->setAccessibleName("RX antenna");
    m_rxAntBtn->setAccessibleDescription("Select receive antenna port");
    m_txAntBtn->setAccessibleName("TX antenna");
    m_txAntBtn->setAccessibleDescription("Select transmit antenna port");
    m_filterWidthLbl->setAccessibleName("Filter width");
    m_qskBtn->setAccessibleName("QSK indicator");
    m_qskBtn->setAccessibleDescription("Full break-in CW indicator");
    m_txBadge->setAccessibleName("TX slice selector");
    m_txBadge->setAccessibleDescription("Click to set this slice as the transmit slice");
    m_modeCombo->setAccessibleName("Operating mode");
    m_modeCombo->setAccessibleDescription("Select operating mode such as USB, LSB, CW, AM, FM");
    m_freqLabel->setAccessibleName("Frequency display");
    m_freqEdit->setAccessibleName("Frequency entry");
    m_freqEdit->setAccessibleDescription("Type a frequency in MHz and press Enter");
    m_stepDown->setAccessibleName("Step size down");
    m_stepLabel->setAccessibleName("Tuning step size");
    m_stepUp->setAccessibleName("Step size up");
    m_filterPassband->setAccessibleName("Filter passband");
    m_filterPassband->setAccessibleDescription("Visual filter passband with draggable edges");
    m_muteBtn->setAccessibleName("Slice audio mute");
    m_afSlider->setAccessibleName("AF gain");
    m_afSlider->setAccessibleDescription("Audio output volume for this slice");
    m_panSlider->setAccessibleName("Audio pan");
    m_panSlider->setAccessibleDescription("Stereo audio pan, left to right");
    m_sqlBtn->setAccessibleName("Squelch mode");
    m_sqlBtn->setAccessibleDescription(
        "Cycle squelch through Off, Manual, and Auto modes");
    m_sqlBtn->setToolTip(
        "Click to cycle:\n"
        "  Off — squelch open, all audio passes\n"
        "  SQL — manual threshold via the slider\n"
        "  AUTO — algorithm tracks the noise floor automatically");
    m_sqlSlider->setAccessibleName("Squelch threshold");
    m_sqlSlider->setAccessibleDescription("Signal level below which audio is muted");
    m_agcCombo->setAccessibleName("AGC mode");
    m_agcCombo->setAccessibleDescription("Automatic gain control speed");
    m_agcTSlider->setAccessibleName("AGC threshold");
    m_agcTSlider->setAccessibleDescription("Maximum gain applied to weak signals");
    m_ritOnBtn->setAccessibleName("RIT toggle");
    m_ritOnBtn->setAccessibleDescription("Receive incremental tuning");
    m_ritZero->setAccessibleName("RIT zero");
    m_ritMinus->setAccessibleName("RIT decrease");
    m_ritLabel->setAccessibleName("RIT offset");
    m_ritPlus->setAccessibleName("RIT increase");
    m_xitOnBtn->setAccessibleName("XIT toggle");
    m_xitOnBtn->setAccessibleDescription("Transmit incremental tuning");
    m_xitZero->setAccessibleName("XIT zero");
    m_xitMinus->setAccessibleName("XIT decrease");
    m_xitLabel->setAccessibleName("XIT offset");
    m_xitPlus->setAccessibleName("XIT increase");
    m_toneModeCmb->setAccessibleName("FM tone mode");
    m_toneValueCmb->setAccessibleName("CTCSS tone frequency");
    m_offsetSpin->setAccessibleName("Repeater offset frequency");
    m_offsetDown->setAccessibleName("Offset down");
    m_simplexBtn->setAccessibleName("Simplex");
    m_offsetUp->setAccessibleName("Offset up");
    m_revBtn->setAccessibleName("Reverse offset");

    root->addStretch();
}

// ─── NR button 3-state sync ──────────────────────────────────────────────────

// DSP sync functions removed — VFO DSP tab and spectrum overlay handle all DSP state

// ─── Squelch 3-way cycle button ──────────────────────────────────────────────

void RxApplet::applySqlModeVisuals()
{
    if (!m_sqlBtn) return;
    // Off: base style, "SQL" label, dim.
    // Manual: green active, "SQL" label.
    // Auto: amber active, "AUTO" label.  Distinct color so the operator can
    // tell at a glance whether they're driving the threshold manually or the
    // algorithm is.
    switch (m_sqlMode) {
    case SqlMode::Off:
        m_sqlBtn->setText("SQL");
        m_sqlBtn->setStyleSheet(QString(kButtonBase) + kDisabledBtn);
        break;
    case SqlMode::Manual:
        m_sqlBtn->setText("SQL");
        m_sqlBtn->setStyleSheet(
            "QPushButton { background: #006040; color: #00ff88; "
            "border: 1px solid #00a060; border-radius: 3px; "
            "font-size: 10px; font-weight: bold; padding: 1px 2px; }"
            "QPushButton:hover { background: #007050; }"
            + kDisabledBtn);
        break;
    case SqlMode::Auto:
        m_sqlBtn->setText("AUTO");
        m_sqlBtn->setStyleSheet(
            "QPushButton { background: #604000; color: #ffb800; "
            "border: 1px solid #906000; border-radius: 3px; "
            "font-size: 10px; font-weight: bold; padding: 1px 2px; }"
            "QPushButton:hover { background: #705000; }"
            + kDisabledBtn);
        break;
    }
    // Slider has two distinct roles depending on mode:
    //   Manual: threshold input (0–100, squelch_level units).
    //   Auto:   dB margin above measured noise floor (5–20).
    //   Off:    disabled.
    // Switching modes resizes the slider's range and restores the
    // appropriate value (live squelch_level for Manual, persisted
    // AutoSqlMarginDb for Auto).  Signals are blocked during the swap
    // so the resize doesn't fire a phantom valueChanged into either path.
    if (m_sqlSlider) {
        QSignalBlocker b(m_sqlSlider);
        switch (m_sqlMode) {
        case SqlMode::Manual: {
            m_sqlSlider->setRange(0, 100);
            // Restore the cached manual level — slice->squelchLevel() may
            // hold a stale Auto-algorithm value that doesn't represent the
            // user's chosen manual threshold.
            m_sqlSlider->setValue(m_sqlManualLevel);
            m_sqlSlider->setEnabled(true);
            m_sqlSlider->setToolTip(
                "Squelch threshold (0–100). Increase to require a stronger "
                "signal before audio opens.");
            break;
        }
        case SqlMode::Auto: {
            m_sqlSlider->setRange(5, 20);
            const int margin = std::clamp(
                AppSettings::instance().value("AutoSqlMarginDb", "10").toInt(),
                5, 20);
            m_sqlSlider->setValue(margin);
            m_sqlSlider->setEnabled(true);
            m_sqlSlider->setToolTip(
                "Auto SQL margin (5–20 dB). dB above the measured noise "
                "floor where the squelch gate opens.");
            break;
        }
        case SqlMode::Off:
            m_sqlSlider->setEnabled(false);
            m_sqlSlider->setToolTip(
                "Squelch threshold. Increase to require a stronger signal "
                "before audio opens.");
            break;
        }
    }
}

void RxApplet::cycleSqlMode()
{
    const SqlMode next =
        (m_sqlMode == SqlMode::Off)    ? SqlMode::Manual :
        (m_sqlMode == SqlMode::Manual) ? SqlMode::Auto   :
                                          SqlMode::Off;
    setSqlMode(next, /*propagateToRadio=*/true);
}

// ── Cross-widget SQL drivers (for VfoWidget mirroring) ──────────────────
//
// VfoWidget hosts a second SQL button+slider that must show the same
// mode and value as the one in RxApplet.  These helpers let it pipe its
// own user events through RxApplet's existing state machine so all the
// persistence (AppSettings LastManualSquelchLevel / AutoSqlMarginDb),
// algorithm enable/disable, and the manual-level cache live in exactly
// one place.  RxApplet emits sqlModeChanged / autoSqlMarginDbChanged
// back so VfoWidget can refresh its UI.

void RxApplet::cycleSqlModeExternal()
{
    cycleSqlMode();
}

int RxApplet::autoSqlMarginDb() const
{
    return std::clamp(
        AppSettings::instance().value("AutoSqlMarginDb", "10").toInt(), 5, 20);
}

void RxApplet::setSqlSliderValueExternal(int v)
{
    if (m_sqlMode == SqlMode::Manual) {
        m_sqlManualLevel = v;
        auto& s = AppSettings::instance();
        s.setValue("LastManualSquelchLevel", QString::number(v));
        s.save();
        if (m_slice)
            m_slice->setSquelch(true, v);
        if (m_sqlSlider) {
            QSignalBlocker b(m_sqlSlider);
            m_sqlSlider->setValue(v);
        }
    } else if (m_sqlMode == SqlMode::Auto) {
        auto& s = AppSettings::instance();
        s.setValue("AutoSqlMarginDb", QString::number(v));
        s.save();
        emit autoSqlMarginDbChanged(v);
        if (m_sqlSlider) {
            QSignalBlocker b(m_sqlSlider);
            m_sqlSlider->setValue(v);
        }
    }
    // Off: ignored — slider is disabled on both UIs.
}

void RxApplet::setSqlMode(SqlMode m, bool propagateToRadio)
{
    if (m == m_sqlMode) return;
    const bool wasAuto = (m_sqlMode == SqlMode::Auto);
    const bool nowAuto = (m == SqlMode::Auto);
    m_sqlMode = m;
    applySqlModeVisuals();

    // Notify any mirroring UI (VfoWidget's SQL button + slider) so it can
    // refresh label / color / slider role to match.  Fires before the
    // radio-push below so listeners see the new mode before any echo.
    emit sqlModeChanged(static_cast<int>(m));

    // Auto state is client-side only; tell the spectrum-side algorithm to
    // start or stop driving the level.
    if (wasAuto != nowAuto)
        emit sqlAutoChanged(nowAuto);

    // Manual / Auto both want the radio squelch ON.  Auto's first level push
    // arrives from the algorithm via autoSquelchLevelSuggested → setSquelch
    // routed through MainWindow; the immediate setSquelch(true, slider) here
    // just makes sure the gate is open while the algorithm warms up.
    if (propagateToRadio && m_slice) {
        const bool sqOn = (m != SqlMode::Off);
        m_slice->setSquelch(sqOn, m_sqlSlider->value());
    }
}

void RxApplet::setAfGain(int pct)
{
    QSignalBlocker b(m_afSlider);
    m_afSlider->setValue(pct);
}

// ─── Slice wiring ─────────────────────────────────────────────────────────────

void RxApplet::setMaxSlices(int maxSlices)
{
    if (!m_sliceBtns.isEmpty()) {
        if (m_sliceBtns.size() == maxSlices) {
            return;
        }
        clearSliceButtons();
    }

    if (maxSlices <= 1) {
        m_sliceTabRow->setVisible(false);
        m_muteAllBtn->hide();
        return;
    }

    // For ≤4 slices: inline on the header row (replace the static badge).
    // For >4 slices (6700): use the separate row above the header.
    const bool useInline = (maxSlices <= 4);
    QHBoxLayout* targetLayout = nullptr;
    int insertIdx = 0;

    if (useInline) {
        m_sliceBadge->setVisible(false);
        targetLayout = m_headerRow;
        // Insert at position 0 (where the badge was)
        insertIdx = 0;
        // Move mute-all button to the right end of the header row.
        // addWidget() reparents it from m_sliceTabRow if needed.
        m_headerRow->addWidget(m_muteAllBtn);
        m_muteAllBtn->show();
    } else {
        auto* layout = qobject_cast<QHBoxLayout*>(m_sliceTabRow->layout());
        // Ensure button is in the tab row (may have been reparented to
        // m_headerRow during a previous inline call). addWidget() is a
        // no-op if already here; otherwise it reparents from m_headerRow.
        layout->addWidget(m_muteAllBtn);
        targetLayout = layout;
        // Insert slice buttons before the stretch so receiver letters stay
        // left-aligned and the mute-all speaker stays right-aligned.
        insertIdx = 0;
        m_sliceTabRow->setVisible(true);
        m_muteAllBtn->show();
    }

    for (int i = 0; i < maxSlices; ++i) {
        auto* btn = new QToolButton;
        btn->setText(QString(QChar('A' + i)));
        btn->setCheckable(true);
        btn->setEnabled(false);  // disabled until a slice occupies this slot
        btn->setFixedSize(22, 20);

        QString color = SliceColorManager::instance().hexActive(i);
        // Three slot states drive QSS via the `slotState` dynamic property:
        //   - "empty"   : no slice in this global slot (current default look)
        //   - "foreign" : another Multi-Flex client owns this slot (#2606,
        //                 dimmer than empty so it reads as "taken, not yours")
        //   - "ours"    : enabled, can be selected; uses the per-slice color
        btn->setStyleSheet(
            QString("QToolButton { background: #2a2a2a; color: %1; border: 1px solid %1; "
                    "border-radius: 3px; font-weight: bold; font-size: 10px; padding: 0; }"
                    "QToolButton:checked { background: %1; color: #000000; }"
                    "QToolButton:disabled { background: #1a1a1a; color: #444444; "
                    "border-color: #333333; }"
                    "QToolButton[slotState=\"foreign\"] { background: #1f1f1f; "
                    "color: #707070; border-color: #555555; }")
                .arg(color));

        m_sliceGroup->addButton(btn, i);
        targetLayout->insertWidget(insertIdx + i, btn);
        m_sliceBtns.append(btn);
    }

    if (!m_sliceButtonClicksConnected) {
        connect(m_sliceGroup, QOverload<int>::of(&QButtonGroup::idClicked),
                this, [this](int /*buttonId*/) {
            auto* btn = qobject_cast<QToolButton*>(m_sliceGroup->checkedButton());
            if (btn) {
                bool ok = false;
                int sliceId = btn->property("sliceId").toInt(&ok);
                if (ok) emit sliceActivationRequested(sliceId);
            }
        });
        m_sliceButtonClicksConnected = true;
    }

    if (!useInline) {
        m_sliceTabRow->setVisible(true);
    }
}

void RxApplet::clearSliceButtons()
{
    if (m_sliceBtns.isEmpty()) {
        return;
    }

    QSignalBlocker blocker(m_sliceGroup);
    while (!m_sliceBtns.isEmpty()) {
        QToolButton* btn = m_sliceBtns.takeLast();
        m_sliceGroup->removeButton(btn);
        delete btn;
    }

    m_sliceTabRow->setVisible(false);
    m_muteAllBtn->hide();
    m_sliceBadge->setVisible(true);
}

void RxApplet::updateSliceButtons(const QList<SliceModel*>& slices, int activeSliceId)
{
    if (m_sliceBtns.isEmpty()) return;

    // Slot states drive both the QSS dynamic property and the per-button
    // configuration branch below.  Scoped to the function so the dispatch
    // is a typed switch rather than string compares.
    enum class SlotState { Ours, Foreign, Empty };

    // Build a map: slot index → SliceModel* for open slices we own.
    QMap<int, SliceModel*> slotToSlice;
    for (auto* s : slices)
        slotToSlice[s->sliceId()] = s;

    QSignalBlocker blocker(m_sliceGroup);
    const auto mode = SliceLabel::currentMode();
    const bool radioIdx = (mode == SliceLabel::Mode::RadioIndexed);

    // In RadioIndexed mode the row is laid out as three groups, left to
    // right:
    //   1. Owned slices, in global-sliceId order, rendered with their
    //      per-client letter + 1-based global-slot subscript (e.g. "A₂").
    //   2. Empty slots — available for the user to claim.  Labelled with
    //      sequential letters continuing after the user's owned set
    //      ("B", "C", … if the user owns one "A" slice already), so the
    //      letter previews what the radio would call the slice once
    //      claimed.
    //   3. Foreign slots — in use by another Multi-Flex client.  Rendered
    //      as "—" so they read as "taken, unavailable" without colliding
    //      with the user's per-client letters.
    //
    // In Global mode the layout is unchanged from today: position == slot.
    QList<int> ownedSlots;
    for (auto it = slotToSlice.constBegin(); it != slotToSlice.constEnd(); ++it)
        ownedSlots.append(it.key());
    std::sort(ownedSlots.begin(), ownedSlots.end());

    QList<int> foreignSlots;
    QList<int> emptySlots;
    if (m_radioModel) {
        for (int i = 0; i < m_sliceBtns.size(); ++i) {
            if (slotToSlice.contains(i)) continue;
            if (m_radioModel->isSlotForeign(i)) foreignSlots.append(i);
            else                                emptySlots.append(i);
        }
    } else {
        for (int i = 0; i < m_sliceBtns.size(); ++i) {
            if (!slotToSlice.contains(i)) emptySlots.append(i);
        }
    }

    auto buildButtonStyle = [](int colourIdx) {
        const QString color = SliceColorManager::instance().hexActive(colourIdx);
        return QString(
            "QToolButton { background: #2a2a2a; color: %1; border: 1px solid %1; "
            "border-radius: 3px; font-weight: bold; font-size: 10px; padding: 0; }"
            "QToolButton:checked { background: %1; color: #000000; }"
            "QToolButton:disabled { background: #1a1a1a; color: #444444; "
            "border-color: #333333; }"
            "QToolButton[slotState=\"foreign\"] { background: #1f1f1f; "
            "color: #707070; border-color: #555555; }")
            .arg(color);
    };

    // Cache the last applied colour index per button so we skip the
    // stylesheet rebuild + setStyleSheet() on every refresh when nothing
    // colour-relevant changed (the loop runs on slot occupancy + letter
    // signals, both of which fire often).
    //
    // Cache key is intentionally colour-index-only.  State transitions
    // (ours / foreign / empty) at the same colour index are handled by
    // the `slotState` dynamic property + QSS attribute selectors via the
    // unpolish/polish call at the bottom of the loop — so the stylesheet
    // string itself doesn't need to change for those.
    auto applyStyleIfChanged = [](QToolButton* btn, int colourIdx,
                                   const QString& stylesheet) {
        const QVariant prev = btn->property("colourIdx");
        if (prev.isValid() && prev.toInt() == colourIdx) return;
        btn->setProperty("colourIdx", colourIdx);
        btn->setStyleSheet(stylesheet);
    };

    for (int btnPos = 0; btnPos < m_sliceBtns.size(); ++btnPos) {
        auto* btn = m_sliceBtns[btnPos];

        // Resolve which slot (if any) this button position represents,
        // and what state it's in.
        int slotId = -1;
        SliceModel* ourSlice = nullptr;
        SlotState state = SlotState::Empty;
        // The displayed sequential letter for empty slots in RadioIndexed
        // mode — only meaningful when state == Empty && radioIdx.
        QChar emptyLetter('?');

        if (radioIdx) {
            const int ownedN = ownedSlots.size();
            const int emptyN = emptySlots.size();
            if (btnPos < ownedN) {
                slotId = ownedSlots[btnPos];
                ourSlice = slotToSlice[slotId];
                state = SlotState::Ours;
            } else if (btnPos < ownedN + emptyN) {
                slotId = emptySlots[btnPos - ownedN];
                state = SlotState::Empty;
                emptyLetter = QChar('A' + btnPos);  // sequential after owned
            } else if (btnPos < ownedN + emptyN + foreignSlots.size()) {
                slotId = foreignSlots[btnPos - ownedN - emptyN];
                state = SlotState::Foreign;
            }
        } else {
            // Global mode: button position == global slot id (original).
            slotId = btnPos;
            if (slotToSlice.contains(btnPos)) {
                ourSlice = slotToSlice[btnPos];
                state = SlotState::Ours;
            } else if (m_radioModel && m_radioModel->isSlotForeign(btnPos)) {
                state = SlotState::Foreign;
            }
        }

        // Apply text + state-specific bits.
        if (state == SlotState::Ours) {
            btn->setText(SliceLabel::unicodeForm(slotId, ourSlice->letter()));
            btn->setEnabled(true);
            btn->setProperty("sliceId", slotId);
            btn->setProperty("slotState", "ours");
            const QChar gLetter('A' + slotId);
            const QString perClient = ourSlice->letter().isEmpty()
                                           ? QString(gLetter)
                                           : ourSlice->letter();
            btn->setToolTip(QString("Slice %1 (global slot %2)")
                                .arg(perClient).arg(slotId + 1));
            btn->setChecked(slotId == activeSliceId);
            // Colour pairs with the displayed letter in RadioIndexed mode.
            const int colourIdx = SliceLabel::displayColorIndex(slotId, ourSlice->letter());
            applyStyleIfChanged(btn, colourIdx, buildButtonStyle(colourIdx));
        } else if (state == SlotState::Foreign) {
            // Foreign slots always render as "—" so they're visually
            // distinguishable from empty slots even with the dim styling
            // — colour-blind users couldn't separate grey/dim letters
            // from dim-letters before (#2606 follow-up).
            btn->setText(QString::fromUtf8("—"));
            btn->setEnabled(false);
            btn->setChecked(false);
            btn->setProperty("sliceId", QVariant());
            btn->setProperty("slotState", "foreign");
            const QString owner = m_radioModel
                                       ? m_radioModel->foreignSliceOwnerStation(slotId)
                                       : QString();
            btn->setToolTip(owner.isEmpty()
                                ? QString("Slot %1 — in use by another client")
                                      .arg(QChar('A' + slotId))
                                : QString("Slot %1 — in use by %2")
                                      .arg(QChar('A' + slotId)).arg(owner));
            // Foreign keeps its global slot colour as the base palette;
            // the slotState QSS override paints it in the dim grey scheme.
            applyStyleIfChanged(btn, slotId, buildButtonStyle(slotId));
        } else {
            // Empty / available slot.  In Global mode show the global
            // letter for the slot.  In RadioIndexed mode show the
            // sequential letter that the radio would assign if the user
            // claimed it (e.g. with one owned "A" slice, empty slots
            // show B, C, D).  Colour follows the displayed letter.
            const int colourPos = (slotId >= 0) ? slotId : btnPos;
            QString glyph;
            int colourIdx = colourPos;
            if (radioIdx) {
                glyph = QString(emptyLetter);
                colourIdx = (emptyLetter >= QChar('A') && emptyLetter <= QChar('H'))
                                ? emptyLetter.unicode() - 'A'
                                : colourPos;
            } else {
                glyph = QString(QChar('A' + colourPos));
            }
            btn->setText(glyph);
            btn->setEnabled(false);
            btn->setChecked(false);
            btn->setProperty("sliceId", QVariant());
            btn->setProperty("slotState", "empty");
            btn->setToolTip(QString("Slot %1 — empty").arg(QChar('A' + colourPos)));
            applyStyleIfChanged(btn, colourIdx, buildButtonStyle(colourIdx));
        }
        // Force a stylesheet re-evaluation so the slotState property change
        // is picked up by the QSS attribute selectors.
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
    }
}

void RxApplet::setSlice(SliceModel* slice)
{
    clearLockedFrequencyFeedback();
    if (m_slice) disconnectSlice(m_slice);
    m_slice = slice;
    if (m_slice) connectSlice(m_slice);
}

void RxApplet::setAntennaList(const QStringList& ants)
{
    if (ants.isEmpty()) return;
    m_antList = ants;
    updateAntennaButtons();
}

void RxApplet::setRadioModel(RadioModel* radioModel)
{
    if (m_radioModel) {
        disconnect(m_radioModel, &RadioModel::antennaAliasesChanged,
                   this, &RxApplet::updateAntennaButtons);
        disconnect(m_radioModel, &RadioModel::slotOccupancyChanged,
                   this, nullptr);
    }
    m_radioModel = radioModel;
    if (m_radioModel) {
        connect(m_radioModel, &RadioModel::antennaAliasesChanged,
                this, &RxApplet::updateAntennaButtons);
        // Slot-occupancy changes (other Multi-Flex clients connecting /
        // disconnecting, slots claimed / released) restyle the slice tab
        // row so foreign slots dim and free slots open back up (#2606).
        connect(m_radioModel, &RadioModel::slotOccupancyChanged,
                this, [this](int /*sliceId*/) {
            if (!m_radioModel) return;
            const int active = m_slice ? m_slice->sliceId() : -1;
            updateSliceButtons(m_radioModel->slices(), active);
        });
    }
    updateAntennaButtons();
}

void RxApplet::setTransmitModel(TransmitModel* txModel)
{
    if (m_txModel) m_txModel->disconnect(this);
    m_txModel = txModel;
    if (!m_txModel) return;

    // Sync QSK indicator from transmit model's break_in state
    {
        QSignalBlocker b(m_qskBtn);
        m_qskBtn->setChecked(m_txModel->cwBreakIn());
    }
    connect(m_txModel, &TransmitModel::phoneStateChanged, this, [this] {
        QSignalBlocker b(m_qskBtn);
        m_qskBtn->setChecked(m_txModel->cwBreakIn());
    });
}

QString RxApplet::antennaMenuLabel(const QString& token,
                                   const QStringList& options) const
{
    if (!m_radioModel)
        return token;
    return m_radioModel->antennaDisplayName(
        token, m_radioModel->antennaAliasNeedsDisambiguation(token, options));
}

QStringList RxApplet::txAntennaOptions() const
{
    QStringList options;
    auto append = [&options](const QString& token) {
        if (!token.isEmpty() && !options.contains(token))
            options.append(token);
    };

    if (m_slice && !m_slice->txAntennaList().isEmpty()) {
        for (const QString& ant : m_slice->txAntennaList())
            append(ant);
        append(m_slice->txAntenna());
        return options;
    }

    for (const QString& ant : m_antList) {
        if (likelyTxAntennaFallbackToken(ant))
            append(ant);
    }
    if (m_slice)
        append(m_slice->txAntenna());
    return options;
}

void RxApplet::updateAntennaButton(QPushButton* button, const QString& token, bool tx)
{
    if (!button)
        return;

    const QString shortLabel = m_radioModel
        ? m_radioModel->antennaShortDisplayName(token, 6)
        : token;
    const QFontMetrics fm(button->font());
    constexpr int kMinWidth = 30;
    constexpr int kMaxWidth = 58;
    constexpr int kPad = 8;
    const QString text = fm.elidedText(shortLabel, Qt::ElideRight, kMaxWidth - kPad);
    button->setText(text);
    button->setFixedWidth(qBound(kMinWidth, fm.horizontalAdvance(text) + kPad, kMaxWidth));

    const QString full = m_radioModel
        ? m_radioModel->antennaDisplayName(token, !m_radioModel->antennaAlias(token).isEmpty())
        : token;
    button->setToolTip(QStringLiteral("%1 antenna port: %2")
                           .arg(tx ? QStringLiteral("Transmit") : QStringLiteral("Receive"),
                                full));
}

void RxApplet::updateAntennaButtons()
{
    if (!m_slice)
        return;
    updateAntennaButton(m_rxAntBtn, m_slice->rxAntenna(), false);
    updateAntennaButton(m_txAntBtn, m_slice->txAntenna(), true);
}

void RxApplet::connectSlice(SliceModel* s)
{
    // ── Header ─────────────────────────────────────────────────────────────

    // Slice badge — display follows SliceLetterDisplay AppSettings (#2606).
    m_sliceBadge->setText(SliceLabel::richText(s->sliceId(), s->letter()));
    const int sid = s->sliceId();
    // Colour pairs with the visible letter — see SliceLabel::displayColorIndex.
    const int colourIdx = SliceLabel::displayColorIndex(sid, s->letter());
    m_sliceBadge->setStyleSheet(
        QString("QLabel { background: %1; color: #000000; "
                "border-radius: 3px; font-weight: bold; font-size: 11px; }")
            .arg(SliceColorManager::instance().hexActive(colourIdx)));

    // Lock
    {
        QSignalBlocker b(m_lockBtn);
        m_lockBtn->setChecked(s->isLocked());
        m_lockBtn->setText(s->isLocked() ? "\U0001F512" : "\U0001F513");
    }
    connect(s, &SliceModel::lockedChanged, this, [this, s](bool locked) {
        QSignalBlocker b(m_lockBtn);
        m_lockBtn->setChecked(locked);
        m_lockBtn->setText(locked ? "\U0001F512" : "\U0001F513");
        if (locked && m_freqStack && m_freqStack->currentIndex() == 1) {
            m_freqEdit->setText(QString::number(s->frequency(), 'f', 6));
            m_freqStack->setCurrentIndex(0);
            m_freqEdit->clearFocus();
        } else if (!locked) {
            clearLockedFrequencyFeedback();
        }
    });

    // Per-client letter refresh — Multi-Flex sessions can deliver
    // index_letter after slice creation (#2606).
    connect(s, &SliceModel::letterChanged, this,
            [this, s](const QString&) {
        m_sliceBadge->setText(SliceLabel::richText(s->sliceId(), s->letter()));
        // Tab row also needs to pick up the new letter on the active
        // slice's button.
        if (m_radioModel) {
            updateSliceButtons(m_radioModel->slices(), s->sliceId());
        }
    });

    // RX antenna
    updateAntennaButton(m_rxAntBtn, s->rxAntenna(), false);
    connect(s, &SliceModel::rxAntennaChanged, this, [this](const QString& ant) {
        updateAntennaButton(m_rxAntBtn, ant, false);
    });

    // TX antenna
    updateAntennaButton(m_txAntBtn, s->txAntenna(), true);
    connect(s, &SliceModel::txAntennaChanged, this, [this](const QString& ant) {
        updateAntennaButton(m_txAntBtn, ant, true);
    });
    connect(s, &SliceModel::txAntennaListChanged,
            this, [this](const QStringList&) { updateAntennaButtons(); });

    // Filter width label
    m_filterWidthLbl->setText(formatFilterWidth(s->filterLow(), s->filterHigh(), s->mode()));

    // QSK
    {
        QSignalBlocker b(m_qskBtn);
        m_qskBtn->setChecked(s->qskOn());
    }
    connect(s, &SliceModel::qskChanged, this, [this](bool on) {
        QSignalBlocker b(m_qskBtn); m_qskBtn->setChecked(on);
    });

    // TX slice badge
    auto updateTxBadge = [this](bool tx) {
        m_txBadge->setStyleSheet(tx
            ? "QPushButton { background: #c03030; color: #ffffff; "
              "border-radius: 3px; border: none; font-weight: bold; font-size: 10px;"
              " padding: 0px; margin: 0px; }"
              "QPushButton:hover { background: #d04040; }"
            : "QPushButton { background: #405060; color: #ffffff; "
              "border-radius: 3px; border: none; font-weight: bold; font-size: 10px;"
              " padding: 0px; margin: 0px; }"
              "QPushButton:hover { background: #506070; }");
    };
    updateTxBadge(s->isTxSlice());
    connect(s, &SliceModel::txSliceChanged, this, updateTxBadge);

    // Mode combo
    {
        QSignalBlocker b(m_modeCombo);
        int idx = m_modeCombo->findText(s->mode());
        if (idx >= 0) m_modeCombo->setCurrentIndex(idx);
    }
    connect(s, &SliceModel::modeListChanged, this, [this](const QStringList& modes) {
        if (modes.isEmpty()) return;          // keep static fallback list (#891)
        QSignalBlocker b(m_modeCombo);
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
    if (!s->modeList().isEmpty()) {
        QSignalBlocker b(m_modeCombo);
        QString cur = m_modeCombo->currentText();
        m_modeCombo->clear();
        m_modeCombo->addItems(s->modeList());
#ifdef HAVE_RADE
        if (m_modeCombo->findText("RADE") < 0)
            m_modeCombo->addItem("RADE");
#endif
        int idx = m_modeCombo->findText(cur);
        if (idx >= 0) m_modeCombo->setCurrentIndex(idx);
    }
    connect(s, &SliceModel::modeChanged, this, [this](const QString& mode) {
        QSignalBlocker b(m_modeCombo);
        int idx = m_modeCombo->findText(mode);
        if (idx >= 0) m_modeCombo->setCurrentIndex(idx);
        updateModeSettings(mode);
    });

    // Initialize filter/step arrays for the current mode
    updateModeSettings(s->mode());

    // Frequency display (XX.XXX.XXX format)
    updateFreqLabel();
    connect(s, &SliceModel::frequencyChanged, this, [this](double) {
        updateFreqLabel();
    });
    connect(s, &SliceModel::tuneBlockedByLock,
            this, &RxApplet::showLockedFrequencyFeedback);

    // ── Filter ─────────────────────────────────────────────────────────────
    updateFilterButtons();
    m_filterPassband->setFilter(s->filterLow(), s->filterHigh());
    m_filterPassband->setMode(s->mode());
    connect(s, &SliceModel::filterChanged, this, [this](int lo, int hi) {
        updateFilterButtons();
        m_filterWidthLbl->setText(formatFilterWidth(lo, hi, m_slice ? m_slice->mode() : QString()));
        m_filterPassband->setFilter(lo, hi);
    });
    connect(s, &SliceModel::modeChanged, this, [this](const QString& mode) {
        m_filterPassband->setMode(mode);
        if (m_slice)
            m_filterWidthLbl->setText(formatFilterWidth(m_slice->filterLow(), m_slice->filterHigh(), mode));
    });

    // AGC mode
    updateAgcCombo();
    connect(s, &SliceModel::agcModeChanged, this, [this](const QString&) {
        updateAgcCombo();
    });

    // AGC threshold / off level — slider switches based on AGC mode
    {
        QSignalBlocker b(m_agcTSlider);
        if (s->agcMode() == "off") {
            m_agcTSlider->setValue(s->agcOffLevel());
            m_agcTSlider->setToolTip(QString("AGC Off Level: %1").arg(s->agcOffLevel()));
        } else {
            m_agcTSlider->setValue(s->agcThreshold());
            m_agcTSlider->setToolTip(QString("AGC Threshold: %1").arg(s->agcThreshold()));
        }
    }
    connect(s, &SliceModel::agcThresholdChanged, this, [this](int v) {
        if (m_slice && m_slice->agcMode() != "off") {
            QSignalBlocker b(m_agcTSlider);
            m_agcTSlider->setValue(v);
            m_agcTSlider->setToolTip(QString("AGC Threshold: %1").arg(v));
        }
    });
    connect(s, &SliceModel::agcOffLevelChanged, this, [this](int v) {
        if (m_slice && m_slice->agcMode() == "off") {
            QSignalBlocker b(m_agcTSlider);
            m_agcTSlider->setValue(v);
            m_agcTSlider->setToolTip(QString("AGC Off Level: %1").arg(v));
        }
    });
    connect(s, &SliceModel::agcModeChanged, this, [this](const QString& mode) {
        if (!m_slice) return;
        QSignalBlocker b(m_agcTSlider);
        if (mode == "off") {
            m_agcTSlider->setValue(m_slice->agcOffLevel());
            m_agcTSlider->setToolTip(QString("AGC Off Level: %1").arg(m_slice->agcOffLevel()));
        } else {
            m_agcTSlider->setValue(m_slice->agcThreshold());
            m_agcTSlider->setToolTip(QString("AGC Threshold: %1").arg(m_slice->agcThreshold()));
        }
    });

    // Audio mute
    {
        QSignalBlocker b(m_muteBtn);
        m_muteBtn->setChecked(s->audioMute());
        m_muteBtn->setText(s->audioMute()
            ? QString::fromUtf8("\xF0\x9F\x94\x87")
            : QString::fromUtf8("\xF0\x9F\x94\x8A"));
    }
    connect(s, &SliceModel::audioMuteChanged, this, [this](bool muted) {
        QSignalBlocker b(m_muteBtn);
        m_muteBtn->setChecked(muted);
        m_muteBtn->setText(muted
            ? QString::fromUtf8("\xF0\x9F\x94\x87")
            : QString::fromUtf8("\xF0\x9F\x94\x8A"));
    });

    // Audio pan
    {
        QSignalBlocker b(m_panSlider);
        m_panSlider->setValue(s->audioPan());
    }
    connect(s, &SliceModel::audioPanChanged, this, [this](int v) {
        QSignalBlocker b(m_panSlider);
        m_panSlider->setValue(v);
    });

    // Squelch — derive 3-way mode from the radio's squelch on/off state.
    // Auto is client-side only and doesn't survive slice switches, so a
    // freshly-selected slice always starts in Off or Manual.  The user can
    // re-enter Auto on the new slice with one click if they want.
    {
        QSignalBlocker b1(m_sqlBtn), b2(m_sqlSlider);
        // Do NOT overwrite m_sqlManualLevel from the slice here — the
        // slice's squelchLevel may have been clobbered by Auto-mode
        // algorithm pushes in the prior session, and we want to keep the
        // user's persisted manual choice as the source of truth across
        // launches.  The slider value is still visually set to the
        // slice's current state below; applySqlModeVisuals (via the
        // setSqlMode call) will overwrite that with m_sqlManualLevel
        // when entering Manual.
        m_sqlSlider->setValue(s->squelchLevel());
        setSqlMode(s->squelchOn() ? SqlMode::Manual : SqlMode::Off,
                   /*propagateToRadio=*/false);
    }
    emit squelchStateChanged(s->squelchOn(), s->squelchLevel());
    // AF gain → radio's per-slice audio_level
    {
        QSignalBlocker sb(m_afSlider);
        m_afSlider->setValue(static_cast<int>(s->audioGain()));
    }
    connect(s, &SliceModel::audioGainChanged, this, [this](float g) {
        QSignalBlocker sb(m_afSlider);
        m_afSlider->setValue(static_cast<int>(g));
    });

    connect(s, &SliceModel::squelchChanged, this, [this](bool on, int level) {
        QSignalBlocker b1(m_sqlBtn), b2(m_sqlSlider);
        // In Auto mode the slider represents the operator-chosen dB margin,
        // NOT the algorithm-suggested threshold — skip the value update so
        // the algorithm's tick-by-tick setSquelch echoes don't overwrite
        // the user's margin choice.  The spectrum-side yellow SQL line
        // still tracks `level` via setSquelchLine.
        if (m_sqlMode != SqlMode::Auto) {
            m_sqlSlider->setValue(level);
            // Keep the Manual-level cache in sync with external squelch
            // changes (TCI client, radio panel knob) so a later Manual
            // re-entry restores the up-to-date value, not a stale one.
            // Persist too so launch-time recall reflects the most recent
            // manual threshold regardless of who set it.
            if (m_sqlMode == SqlMode::Manual) {
                m_sqlManualLevel = level;
                auto& settings = AppSettings::instance();
                settings.setValue("LastManualSquelchLevel",
                                  QString::number(level));
                settings.save();
            }
        }
        // Auto drives setSquelch every algorithm tick; don't demote out of
        // Auto when the echo arrives with squelch-on.  Only flip the mode
        // when the radio reports squelch-off (operator hit it from somewhere
        // else) or when we were Off and squelch came on (external SQL on).
        if (!on && m_sqlMode != SqlMode::Off)
            setSqlMode(SqlMode::Off, /*propagateToRadio=*/false);
        else if (on && m_sqlMode == SqlMode::Off && m_sqlBtn->isEnabled())
            setSqlMode(SqlMode::Manual, /*propagateToRadio=*/false);
        emit squelchStateChanged(on, level);
    });

    // DSP toggles removed — use VFO DSP tab or spectrum overlay

    // RIT
    {
        QSignalBlocker b(m_ritOnBtn);
        m_ritOnBtn->setChecked(s->ritOn());
        m_ritLabel->setText(formatHz(s->ritFreq()));
    }
    connect(s, &SliceModel::ritChanged, this, [this](bool on, int hz) {
        QSignalBlocker b(m_ritOnBtn);
        m_ritOnBtn->setChecked(on);
        m_ritLabel->setText(formatHz(hz));
    });

    // XIT
    {
        QSignalBlocker b(m_xitOnBtn);
        m_xitOnBtn->setChecked(s->xitOn());
        m_xitLabel->setText(formatHz(s->xitFreq()));
    }
    connect(s, &SliceModel::xitChanged, this, [this](bool on, int hz) {
        QSignalBlocker b(m_xitOnBtn);
        m_xitOnBtn->setChecked(on);
        m_xitLabel->setText(formatHz(hz));
    });

    // ── FM duplex/repeater ────────────────────────────────────────────────

    // Tone mode
    {
        QSignalBlocker b(m_toneModeCmb);
        int idx = m_toneModeCmb->findData(s->fmToneMode());
        if (idx >= 0) m_toneModeCmb->setCurrentIndex(idx);
        m_toneValueCmb->setEnabled(s->fmToneMode() == "ctcss_tx");
    }
    connect(s, &SliceModel::fmToneModeChanged, this, [this](const QString& mode) {
        QSignalBlocker b(m_toneModeCmb);
        int idx = m_toneModeCmb->findData(mode);
        if (idx >= 0) m_toneModeCmb->setCurrentIndex(idx);
        m_toneValueCmb->setEnabled(mode == "ctcss_tx");
    });

    // Tone value
    {
        QSignalBlocker b(m_toneValueCmb);
        for (int i = 0; i < m_toneValueCmb->count(); ++i) {
            if (m_toneValueCmb->itemData(i).toString() == s->fmToneValue()) {
                m_toneValueCmb->setCurrentIndex(i);
                break;
            }
        }
    }
    connect(s, &SliceModel::fmToneValueChanged, this, [this](const QString& val) {
        QSignalBlocker b(m_toneValueCmb);
        for (int i = 0; i < m_toneValueCmb->count(); ++i) {
            if (m_toneValueCmb->itemData(i).toString() == val) {
                m_toneValueCmb->setCurrentIndex(i);
                break;
            }
        }
    });

    // Repeater offset frequency
    {
        QSignalBlocker b(m_offsetSpin);
        m_offsetSpin->setValue(s->fmRepeaterOffsetFreq());
    }
    connect(s, &SliceModel::fmRepeaterOffsetFreqChanged, this, [this](double mhz) {
        QSignalBlocker b(m_offsetSpin);
        m_offsetSpin->setValue(mhz);
    });

    // Offset direction
    updateOffsetDirButtons();
    connect(s, &SliceModel::repeaterOffsetDirChanged, this, [this](const QString&) {
        updateOffsetDirButtons();
    });

    // REV — derive from txOffsetFreq sign vs direction
    {
        QSignalBlocker b(m_revBtn);
        m_revBtn->setChecked(false);  // REV state not persisted by radio
    }

    // Step size — sync from radio's per-slice step and step_list
    syncStepFromSlice(s->stepHz(), s->stepList());
    connect(s, &SliceModel::stepChanged, this, &RxApplet::syncStepFromSlice);
}

void RxApplet::disconnectSlice(SliceModel* s)
{
    s->disconnect(this);
}

// ─── Private helpers ──────────────────────────────────────────────────────────

QString RxApplet::formatHz(int hz)
{
    return (hz >= 0 ? "+" : "") + QString::number(hz) + " Hz";
}

QString RxApplet::formatFilterWidth(int lo, int hi, const QString& mode)
{
    int w;
    if (mode == "USB" || mode == "DIGU" || mode == "FDV" || mode == "FDVU" || mode == "NT")
        w = hi;
    else if (mode == "LSB" || mode == "DIGL" || mode == "FDVL")
        w = std::abs(lo);
    else
        w = hi - lo;
    if (w <= 0) return "?";
    if (w >= 1000) return QString::number(w / 1000.0, 'f', 1) + "K";
    return QString::number(w);
}

void RxApplet::applyFilterPreset(int widthHz)
{
    if (!m_slice) return;

    int lo, hi;
    const QString& mode = m_slice->mode();

    if (mode == "DIGU") {
        if (widthHz < 3000) {
            int offset = m_slice->diguOffset();
            lo = offset - widthHz / 2;
            hi = offset + widthHz / 2;
            if (lo < 95) { hi += (95 - lo); lo = 95; }
        } else {
            lo = 95; hi = widthHz;
        }
    } else if (mode == "DIGL") {
        if (widthHz < 3000) {
            int offset = m_slice->diglOffset();
            hi = -offset + widthHz / 2;
            lo = -offset - widthHz / 2;
            if (hi > -95) { lo -= (hi + 95); hi = -95; }
        } else {
            lo = -widthHz; hi = -95;
        }
    } else if (mode == "LSB") {
        lo = -widthHz;
        hi = -95;
    } else if (mode == "RTTY") {
        // RTTY: RF_frequency = mark. Filter is relative to mark.
        // Space is at -rttyShift. Passband should encompass both tones.
        // Expand symmetrically around the midpoint between mark(0) and space(-shift).
        int shift = m_slice ? m_slice->rttyShift() : 170;
        int mid = -shift / 2;  // midpoint between mark(0) and space(-shift)
        lo = mid - widthHz / 2;
        hi = mid + widthHz / 2;
    } else if (mode == "CW" || mode == "CWL") {
        // Centered on carrier — the radio's BFO/demodulator applies the
        // pitch offset internally so signals at 0 Hz are heard at the sidetone.
        lo = -widthHz / 2;
        hi =  widthHz / 2;
    } else if (mode == "AM" || mode == "SAM" || mode == "DSB") {
        // Double-sideband: split width equally around carrier
        lo = -(widthHz / 2);
        hi =  (widthHz / 2);
    } else if (mode == "FDVL") {
        lo = -widthHz; hi = -95;
    } else {
        // USB, FDVU, FDV, etc. — low cut at 95 Hz to reject carrier/hum
        lo = 95;
        hi = widthHz;
    }

    m_slice->setFilterWidth(lo, hi);
}

void RxApplet::stepFilterWidth(int direction)
{
    if (!m_slice || m_filterWidths.isEmpty() || direction == 0) return;

    const int currentWidth = m_slice->filterHigh() - m_slice->filterLow();
    int idx = 0;
    int bestDist = std::numeric_limits<int>::max();
    for (int i = 0; i < m_filterWidths.size(); ++i) {
        const int dist = std::abs(currentWidth - m_filterWidths[i]);
        if (dist < bestDist) { bestDist = dist; idx = i; }
    }
    const int next = std::clamp(idx + (direction > 0 ? 1 : -1),
                                0, static_cast<int>(m_filterWidths.size()) - 1);
    if (next == idx && bestDist == 0) return;
    applyFilterPreset(m_filterWidths[next]);
}

void RxApplet::updateFilterButtons()
{
    if (!m_slice) return;

    // Reload presets from AppSettings in case VfoWidget changed them.
    // RxApplet shows at most 6 (first 6 of the shared preset list).
    static constexpr int kMaxRxFilters = 6;
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
            if (loadedWidths.size() >= kMaxRxFilters) break;
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

    // Find the single closest matching filter preset
    int bestIdx = -1;
    int bestDist = INT_MAX;
    if (width >= 0) {
        for (int i = 0; i < m_filterWidths.size(); ++i) {
            int dist = std::abs(width - m_filterWidths[i]);
            if (dist < bestDist) { bestDist = dist; bestIdx = i; }
        }
        // Only highlight if reasonably close (within 10% of the preset width)
        if (bestIdx >= 0 && bestDist > m_filterWidths[bestIdx] / 10)
            bestIdx = -1;
    }

    for (int i = 0; i < m_filterBtns.size(); ++i) {
        QSignalBlocker sb(m_filterBtns[i]);
        m_filterBtns[i]->setChecked(i == bestIdx);
    }
}

QString RxApplet::formatStepLabel(int hz)
{
    if (hz >= 1000000) return QString("%1M").arg(hz / 1000000.0, 0, 'f',
                                                  (hz % 1000000) ? 1 : 0);
    if (hz >= 1000)    return QString("%1K").arg(hz / 1000.0, 0, 'f',
                                                  (hz % 1000) ? 1 : 0);
    return QString::number(hz);
}

void RxApplet::updateModeSettings(const QString& mode)
{
    const auto& settings = modeSettingsFor(mode);

    const bool isFM = (mode == "FM" || mode == "NFM" || mode == "DFM");

    // Load custom filter presets from AppSettings, fall back to defaults.
    // RxApplet shows at most 6 (first 6 of VfoWidget's 8).
    // Storage format mirrors VfoWidget — "width" or "lo:hi" entries (#2259).
    static constexpr int kMaxRxFilters = 6;
    QString key = QStringLiteral("FilterPresets_%1").arg(mode);
    QString saved = AppSettings::instance().value(key, "").toString();
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
            if (m_filterWidths.size() >= kMaxRxFilters) break;
        }
    }
    if (m_filterWidths.isEmpty()) {
        m_filterWidths = settings.filterWidths;
        m_filterCustomLo.fill(INT_MIN, m_filterWidths.size());
        m_filterCustomHi.fill(INT_MIN, m_filterWidths.size());
    }
    rebuildFilterButtons();
    m_filterContainer->setVisible(!m_filterWidths.isEmpty() && !isFM);

    // Show/hide FM vs SSB/CW controls
    m_fmContainer->setVisible(isFM);
    m_agcContainer->setVisible(!isFM);
    m_ritContainer->setVisible(!isFM);
    m_xitContainer->setVisible(!isFM);

    // QSK visibility — only meaningful in CW mode
    m_qskBtn->setVisible(mode == "CW");

    // Disable squelch in digital, RTTY, and CW modes
    // Digital/RTTY: audio feeds external decoders via DAX, SQL not meaningful
    //   and gates weak FSK signals (#2504)
    // CW: radio locks squelch on at fixed level, rejects changes
    bool sqlDisabled = (mode == "DIGU" || mode == "DIGL" || mode == "NT"
                        || mode == "RTTY"
                        || mode == "CW" || mode == "CWL");
    m_sqlBtn->setEnabled(!sqlDisabled);
    // Slider enabled when the mode allows squelch AND we're not in SqlMode::Off.
    // Manual mode = threshold input; Auto mode = dB margin input.
    m_sqlSlider->setEnabled(!sqlDisabled && m_sqlMode != SqlMode::Off);
    if (sqlDisabled && m_slice) {
        if (m_slice->squelchOn() || m_sqlMode == SqlMode::Auto) {
            m_savedSquelchOn = true;
            if (mode == "DIGU" || mode == "DIGL" || mode == "NT" || mode == "RTTY") {
                // Only send squelch off for digital/RTTY modes; CW is radio-managed
                m_slice->setSquelch(false, m_slice->squelchLevel());
                setSqlMode(SqlMode::Off, /*propagateToRadio=*/false);
            }
        }
    } else if (!sqlDisabled && m_slice && m_savedSquelchOn) {
        m_savedSquelchOn = false;
        m_slice->setSquelch(true, m_slice->squelchLevel());
        setSqlMode(SqlMode::Manual, /*propagateToRadio=*/false);
    }

    // Step sizes are radio-authoritative — driven by SliceModel::stepChanged
    // signal connected in connectSlice(). No client-side step update here.

    // Refresh filter highlight for current slice filter
    if (m_slice) updateFilterButtons();
}

void RxApplet::rebuildFilterButtons()
{
    // Remove old buttons
    for (auto* btn : m_filterBtns) delete btn;
    m_filterBtns.clear();

    // Create new buttons matching current mode's filter widths
    for (int i = 0; i < m_filterWidths.size(); ++i) {
        const int w = m_filterWidths[i];
        auto* btn = mkToggle(formatStepLabel(w));
        btn->setStyleSheet(QString(kButtonBase) + kBlueActive);
        connect(btn, &QPushButton::clicked, this, [this, i](bool) {
            if (!m_slice) return;
            if (m_filterCustomLo[i] != INT_MIN) {
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
                const auto& factory = modeSettingsFor(m_slice->mode()).filterWidths;
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
        m_filterGrid->addWidget(btn, i / 3, i % 3);
    }
}

void RxApplet::saveFilterPresets()
{
    if (!m_slice) return;
    const QString key = QStringLiteral("FilterPresets_%1").arg(m_slice->mode());
    auto& s = AppSettings::instance();

    // Read existing entries verbatim so VfoWidget's 7th/8th slots survive
    // (they may include "lo:hi" custom-edge entries we don't want to lose).
    QStringList full;
    QString existing = s.value(key, "").toString();
    if (!existing.isEmpty())
        full = existing.split(',', Qt::SkipEmptyParts);

    // Encode our slots (up to 6) — emit "lo:hi" when custom edges set,
    // bare width otherwise. (#2259)
    auto encode = [this](int i) {
        if (m_filterCustomLo[i] != INT_MIN)
            return QString("%1:%2").arg(m_filterCustomLo[i]).arg(m_filterCustomHi[i]);
        return QString::number(m_filterWidths[i]);
    };

    for (int i = 0; i < m_filterWidths.size(); ++i) {
        if (i < full.size())
            full[i] = encode(i);
        else
            full.append(encode(i));
    }

    s.setValue(key, full.join(','));
    s.save();
}

void RxApplet::rebuildStepSizes()
{
    // Step buttons are already connected; just clamp index
    if (m_stepIdx >= m_stepSizes.size())
        m_stepIdx = m_stepSizes.size() - 1;
    if (m_stepIdx < 0) m_stepIdx = 0;
}

void RxApplet::cycleStepUp()
{
    if (m_stepSizes.isEmpty()) return;
    m_stepIdx = (m_stepIdx + 1) % m_stepSizes.size();
    m_stepLabel->setText(formatStepLabel(m_stepSizes[m_stepIdx]));
    emit stepSizeChanged(m_stepSizes[m_stepIdx]);
}

void RxApplet::cycleStepDown()
{
    if (m_stepIdx > 0) {
        m_stepIdx--;
        m_stepLabel->setText(formatStepLabel(m_stepSizes[m_stepIdx]));
        emit stepSizeChanged(m_stepSizes[m_stepIdx]);
    }
}

void RxApplet::setInitialStepSize(int hz)
{
    if (m_stepSizes.isEmpty()) return;
    int bestIdx = 0;
    int bestDist = std::abs(m_stepSizes[0] - hz);
    for (int i = 1; i < m_stepSizes.size(); ++i) {
        int dist = std::abs(m_stepSizes[i] - hz);
        if (dist < bestDist) { bestDist = dist; bestIdx = i; }
    }
    m_stepIdx = bestIdx;
    m_stepLabel->setText(formatStepLabel(m_stepSizes[m_stepIdx]));
}

void RxApplet::syncStepFromSlice(int stepHz, const QVector<int>& stepList)
{
    // Update step list if the radio sent one (mode-specific)
    if (!stepList.isEmpty() && stepList != m_stepSizes) {
        m_stepSizes = stepList;
    }
    // Find closest matching step index
    if (m_stepSizes.isEmpty()) return;
    int bestIdx = 0;
    int bestDist = std::abs(m_stepSizes[0] - stepHz);
    for (int i = 1; i < m_stepSizes.size(); ++i) {
        int dist = std::abs(m_stepSizes[i] - stepHz);
        if (dist < bestDist) { bestDist = dist; bestIdx = i; }
    }
    m_stepIdx = bestIdx;
    m_stepLabel->setText(formatStepLabel(m_stepSizes[m_stepIdx]));

    // Notify SpectrumWidget so scroll-to-tune uses the radio's step size
    emit stepSizeChanged(m_stepSizes[m_stepIdx]);
}

void RxApplet::updateAgcCombo()
{
    const QString cur = m_slice ? m_slice->agcMode() : "";
    QSignalBlocker sb(m_agcCombo);
    for (int i = 0; i < m_agcCombo->count(); ++i) {
        if (m_agcCombo->itemData(i).toString() == cur) {
            m_agcCombo->setCurrentIndex(i);
            break;
        }
    }
}

void RxApplet::updateOffsetDirButtons()
{
    const QString dir = m_slice ? m_slice->repeaterOffsetDir() : "simplex";
    QSignalBlocker b1(m_offsetDown), b2(m_simplexBtn), b3(m_offsetUp);
    m_offsetDown->setChecked(dir == "down");
    m_simplexBtn->setChecked(dir == "simplex");
    m_offsetUp->setChecked(dir == "up");
}

void RxApplet::applyOffsetDir(const QString& dir)
{
    if (!m_slice) return;
    m_slice->setRepeaterOffsetDir(dir);

    // Compute and apply tx_offset_freq
    const double offset = m_slice->fmRepeaterOffsetFreq();
    if (dir == "up")
        m_slice->setTxOffsetFreq(offset);
    else if (dir == "down")
        m_slice->setTxOffsetFreq(-offset);
    else
        m_slice->setTxOffsetFreq(0.0);

    // Clear REV when direction changes
    QSignalBlocker b(m_revBtn);
    m_revBtn->setChecked(false);

    updateOffsetDirButtons();
}

bool RxApplet::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_freqEdit
        && (ev->type() == QEvent::ShortcutOverride
            || ev->type() == QEvent::KeyPress)) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if ((ke->key() == Qt::Key_Escape || ke->key() == Qt::Key_Cancel)
            && m_freqStack->currentIndex() == 1) {
            if (m_slice)
                m_freqEdit->setText(QString::number(m_slice->frequency(), 'f', 6));
            m_freqStack->setCurrentIndex(0);
            m_freqEdit->clearFocus();
            ev->accept();
            return true;
        }
    }

    // Double-click frequency label → inline edit
    if (obj == m_freqLabel && ev->type() == QEvent::MouseButtonDblClick) {
        if (m_slice && m_slice->isLocked()) {
            m_slice->notifyTuneBlockedByLock();
            return true;
        }

        if (m_slice) {
            m_freqEdit->setText(QString::number(m_slice->frequency(), 'f', 6));
            m_freqEdit->selectAll();
        }
        m_freqStack->setCurrentIndex(1);
        m_freqEdit->setFocus();
        return true;
    }

    if (obj == m_freqLabel && ev->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(ev);
        if (m_slice && m_slice->isLocked()) {
            m_slice->notifyTuneBlockedByLock();
            we->accept();
            return true;
        }

        // Clamp to ±1: KDE/Cinnamon send 960 per notch (#504)
        const int raw = we->angleDelta().y() / 120;
        const int steps = qBound(-1, raw, 1);
        if (steps == 0 || !m_slice) { we->ignore(); return true; }

        // Determine which character the cursor is over.
        // Format is "N.NNN.NNN" where the MHz part varies in width (1-3 digits).
        // We count digits right-to-left from the end so place values are stable
        // regardless of how many MHz digits are shown.
        const QString text = m_freqLabel->text();
        const QFontMetrics fm(m_freqLabel->font());
        const int textWidth = fm.horizontalAdvance(text);
        const int rightPad = 1; // matches stylesheet padding
        const int textX0 = m_freqLabel->width() - textWidth - rightPad;
        const int mx = static_cast<int>(we->position().x()) - textX0;

        double place = 0.0;
        if (mx >= 0 && mx < textWidth) {
            // Find which character index the cursor is over
            int cumX = 0;
            int hitIdx = -1;
            for (int i = 0; i < text.size(); ++i) {
                int cw = fm.horizontalAdvance(text[i]);
                if (mx < cumX + cw) { hitIdx = i; break; }
                cumX += cw;
            }
            if (hitIdx >= 0 && text[hitIdx] != '.') {
                // Count digit position from the right end (0 = ones Hz)
                int digitsFromRight = 0;
                for (int i = text.size() - 1; i > hitIdx; --i)
                    if (text[i] != '.') ++digitsFromRight;
                // digitsFromRight: 0=1Hz, 1=10Hz, 2=100Hz, 3=1kHz, ... 8=100MHz
                place = std::pow(10.0, digitsFromRight) / 1.0e6; // in MHz
            }
        }

        // Fall back to step size if cursor is on a dot or outside digits
        if (place == 0.0)
            place = m_stepSizes[m_stepIdx] / 1.0e6;

        m_slice->setFrequency(m_slice->frequency() + place * steps);
        we->accept();
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}

void RxApplet::updateFreqLabel()
{
    if (!m_slice)
        return;

    if (m_showingLockedFrequencyFeedback) {
        m_freqLabel->setText(QStringLiteral("LOCKED"));
        return;
    }

    long long hz = static_cast<long long>(std::round(m_slice->frequency() * 1e6));
    int mhzPart  = static_cast<int>(hz / 1000000);
    int khzPart  = static_cast<int>((hz / 1000) % 1000);
    int hzPart   = static_cast<int>(hz % 1000);
    m_freqLabel->setText(QString("%1.%2.%3")
        .arg(mhzPart)
        .arg(khzPart, 3, 10, QChar('0'))
        .arg(hzPart, 3, 10, QChar('0')));
}

void RxApplet::showLockedFrequencyFeedback()
{
    if (!m_slice || !m_slice->isLocked())
        return;

    if (m_freqStack && m_freqStack->currentIndex() == 1) {
        m_freqEdit->setText(QString::number(m_slice->frequency(), 'f', 6));
        m_freqStack->setCurrentIndex(0);
        m_freqEdit->clearFocus();
    }

    m_showingLockedFrequencyFeedback = true;
    updateFreqLabel();
    m_lockedFrequencyTimer.start(kLockedFrequencyFeedbackMs);
}

void RxApplet::clearLockedFrequencyFeedback()
{
    if (!m_showingLockedFrequencyFeedback)
        return;

    m_lockedFrequencyTimer.stop();
    m_showingLockedFrequencyFeedback = false;
    updateFreqLabel();
}

} // namespace AetherSDR
#include "moc_GuardedSlider.cpp"
