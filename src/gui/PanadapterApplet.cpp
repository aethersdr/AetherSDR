#include "PanadapterApplet.h"
#include "CallsignCard.h"
#include "CwDecodeSettings.h"
#include "FramelessMoveHelper.h"
#include "GuardedSlider.h"
#include "RangeSlider.h"
#include "SliceLabel.h"
#include "SpectrumWidget.h"
#include "Theme.h"
#include "core/AppSettings.h"
#include "core/SettingsHelpers.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QSlider>
#include <QEvent>
#include <QLabel>
#include <QDateTime>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QTextEdit>
#include <QWindow>
#include <QGuiApplication>
#include <QClipboard>
#include "core/ThemeManager.h"

#include <algorithm>

namespace AetherSDR {

PanadapterApplet::PanadapterApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/panadapter"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── Title bar (16px gradient, matching applet style) ─────────────────
    m_titleBar = new QWidget;
    m_titleBar->setFixedHeight(16);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_titleBar, "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "stop:0 {{color.text.disabled}}, stop:0.5 {{color.background.1}}, stop:1 #1a2a38); "
        "border-bottom: 1px solid #0a1a28; }");
    m_titleBar->installEventFilter(this);  // drag-to-move when floating
    auto* titleBar = m_titleBar;

    auto* barLayout = new QHBoxLayout(titleBar);
    barLayout->setContentsMargins(6, 1, 4, 1);
    barLayout->setSpacing(2);

    // Drag grip dots (matching applet title bar style)
    auto* grip = new QLabel(QString::fromUtf8("\xe2\x8b\xae\xe2\x8b\xae"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(grip, "QLabel { background: transparent; color: {{color.text.label}}; font-size: 10px; }");
    barLayout->addWidget(grip);

    m_titleLabel = new QLabel("Slice A");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_titleLabel, "QLabel { background: transparent; color: {{color.text.secondary}}; "
                                "font-size: 10px; font-weight: bold; }");
    m_titleLabel->setTextFormat(Qt::RichText);  // slice letter may be HTML (#2606)
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
    // Restore persisted display preferences (#3628): font size and panel
    // height are user-tunable for readability and history depth.
    m_cwFontPx     = std::clamp(CwDecodeSettings::fontPx(), 8, 32);
    m_cwPanelHeight = std::clamp(CwDecodeSettings::panelHeight(), 60, 600);

    m_cwPanel = new QWidget(this);
    m_cwPanel->setObjectName("cwDecodePanel");  // addressable for automation (#3628/#3646)
    m_cwPanel->setCursor(Qt::ArrowCursor);  // prevent invisible cursor from native-window parent (#1096)
    m_cwPanel->setFixedHeight(m_cwPanelHeight);
    m_cwPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_cwPanel, "QWidget { background: {{color.background.0}}; border-top: 1px solid {{color.background.1}}; }");

    auto* cwLayout = new QVBoxLayout(m_cwPanel);
    cwLayout->setContentsMargins(4, 0, 4, 2);
    cwLayout->setSpacing(1);

    // Drag-grip along the top edge — drag up/down to resize the panel and
    // reveal more decoded-text history (#3628).  A thin strip rather than a
    // QSplitter so the GPU SpectrumWidget (QRhiWidget) is never reparented.
    m_cwGrip = new QWidget(m_cwPanel);
    m_cwGrip->setObjectName("cwResizeGrip");
    m_cwGrip->setFixedHeight(4);
    m_cwGrip->setCursor(Qt::SizeVerCursor);
    m_cwGrip->setToolTip("Drag to resize the CW decode panel");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_cwGrip,
        "QWidget { background: {{color.background.2}}; }");
    m_cwGrip->installEventFilter(this);
    cwLayout->addWidget(m_cwGrip);

    // Stats bar: pitch, speed, clear button
    auto* cwBar = new QHBoxLayout;
    cwBar->setSpacing(6);
    auto* cwTitle = new QLabel("CW");
    AetherSDR::ThemeManager::instance().applyStyleSheet(cwTitle, "QLabel { color: {{color.accent}}; font-size: 10px; font-weight: bold; background: transparent; }");
    cwBar->addWidget(cwTitle);
    auto* cwHint = new QLabel("(requires PC Audio)");
    AetherSDR::ThemeManager::instance().applyStyleSheet(cwHint, "QLabel { color: {{color.meter.bar.fill}}; font-size: 9px; background: transparent; }");
    cwBar->addWidget(cwHint);

    m_cwStatsLabel = new QLabel;
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_cwStatsLabel, "QLabel { color: {{color.text.label}}; font-size: 10px; background: transparent; }");
    m_cwStatsLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_cwStatsLabel->setFixedWidth(m_cwStatsLabel->fontMetrics().horizontalAdvance("1200 Hz  120 WPM"));
    cwBar->addWidget(m_cwStatsLabel);

    // Sensitivity slider — filters low-confidence decodes
    auto* sensLabel = new QLabel("Sens:");
    AetherSDR::ThemeManager::instance().applyStyleSheet(sensLabel, "QLabel { color: {{color.text.label}}; font-size: 9px; background: transparent; }");
    cwBar->addWidget(sensLabel);
    m_cwSensSlider = new GuardedSlider(Qt::Horizontal);
    m_cwSensSlider->setRange(0, 100);  // 0=show everything, 100=only high confidence
    int savedSens = AppSettings::instance().value("CwDecoderSensitivity", "30").toString().toInt();
    m_cwSensSlider->setValue(savedSens);
    m_cwSensSlider->setFixedWidth(60);
    applyPrimarySliderStyle(m_cwSensSlider);
    m_cwCostThreshold = 1.0f - (savedSens / 100.0f) * 0.9f;
    connectSliderSetting(m_cwSensSlider,
        [this](int v) {
            // Map 0-100 slider to 1.0-0.1 cost threshold (inverted: higher sens = lower threshold)
            m_cwCostThreshold = 1.0f - (v / 100.0f) * 0.9f;
        },
        [](int v) {
            auto& settings = AppSettings::instance();
            settings.setValue("CwDecoderSensitivity", QString::number(v));
            settings.save();
        });
    cwBar->addWidget(m_cwSensSlider);

    // Lock Pitch button
    m_lockPitchBtn = new QPushButton("\xF0\x9F\x94\x92P");  // 🔒P
    m_lockPitchBtn->setCheckable(true);
    m_lockPitchBtn->setFixedSize(28, 16);
    m_lockPitchBtn->setToolTip("Lock decoder pitch to current frequency");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_lockPitchBtn, "QPushButton { background: {{color.background.1}}; color: {{color.text.label}}; border: 1px solid {{color.background.1}};"
        " border-radius: 2px; font-size: 8px; padding: 0; }"
        "QPushButton:checked { color: {{color.accent}}; border-color: {{color.accent}}; }"
        "QPushButton:hover { color: {{color.text.primary}}; }");
    cwBar->addWidget(m_lockPitchBtn);

    // Lock Speed button
    m_lockSpeedBtn = new QPushButton("\xF0\x9F\x94\x92S");  // 🔒S
    m_lockSpeedBtn->setCheckable(true);
    m_lockSpeedBtn->setFixedSize(28, 16);
    m_lockSpeedBtn->setToolTip("Lock decoder speed to current WPM");
    m_lockSpeedBtn->setStyleSheet(m_lockPitchBtn->styleSheet());
    cwBar->addWidget(m_lockSpeedBtn);

    // Pitch range — double-handle slider, label embedded in widget
    m_pitchRangeSlider = new RangeSlider(300, 1200, 500, 700, "Pitch", {}, this);
    m_pitchRangeSlider->setFixedWidth(195);
    m_pitchRangeSlider->setToolTip("Decoder pitch search range (Hz)");
    cwBar->addWidget(m_pitchRangeSlider);
    connect(m_pitchRangeSlider, &RangeSlider::rangeChanged, this, [this](int lo, int hi) {
        emit pitchRangeChanged(lo, hi);
    });

    // WPM range — double-handle slider, label embedded in widget
    m_speedRangeSlider = new RangeSlider(5, 60, 15, 40, "WPM", {}, this);
    m_speedRangeSlider->setFixedWidth(195);
    m_speedRangeSlider->setToolTip("Decoder speed search range (WPM)");
    cwBar->addWidget(m_speedRangeSlider);
    connect(m_speedRangeSlider, &RangeSlider::rangeChanged, this, [this](int lo, int hi) {
        emit speedRangeChanged(lo, hi);
    });

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

    // Font-size controls — adjust and persist the decoded-text size (#3628).
    auto* fontDownBtn = new QPushButton("A-");
    fontDownBtn->setObjectName("cwFontDown");
    fontDownBtn->setToolTip("Decrease decoded-text font size");
    fontDownBtn->setStyleSheet(cwBtnStyle);
    connect(fontDownBtn, &QPushButton::clicked, this, [this] { adjustCwFont(-1); });
    cwBar->addWidget(fontDownBtn);

    auto* fontUpBtn = new QPushButton("A+");
    fontUpBtn->setObjectName("cwFontUp");
    fontUpBtn->setToolTip("Increase decoded-text font size");
    fontUpBtn->setStyleSheet(cwBtnStyle);
    connect(fontUpBtn, &QPushButton::clicked, this, [this] { adjustCwFont(+1); });
    cwBar->addWidget(fontUpBtn);

    auto* clearBtn = new QPushButton("CLR");
    clearBtn->setStyleSheet(cwBtnStyle);
    connect(clearBtn, &QPushButton::clicked, this, &PanadapterApplet::clearCwText);
    cwBar->addWidget(clearBtn);

    auto* closeBtn = new QPushButton("\u2715");
    closeBtn->setToolTip("Close CW decoder");
    AetherSDR::ThemeManager::instance().applyStyleSheet(closeBtn, "QPushButton { background: {{color.background.1}}; color: {{color.text.secondary}}; border: 1px solid {{color.background.1}};"
        " border-radius: 2px; font-size: 9px; font-weight: bold;"
        " padding: 1px 6px; }"
        "QPushButton:hover { color: #ff6060; background: {{color.background.1}}; }");
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
    applyCwFont();  // font-size driven by persisted m_cwFontPx (#3628)
    m_cwText->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_cwText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_cwText->setWordWrapMode(QTextOption::WrapAnywhere);
    m_cwText->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_cwText, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu *menu = m_cwText->createStandardContextMenu();
        menu->addSeparator();
        menu->addAction(tr("Increase font size"), this, [this] { adjustCwFont(+1); });
        menu->addAction(tr("Decrease font size"), this, [this] { adjustCwFont(-1); });
        menu->addSeparator();
        menu->addAction(tr("Clear"), this, &PanadapterApplet::clearCwText);
        menu->exec(m_cwText->mapToGlobal(pos));
        delete menu;
    });
    // Decoded text + contact card side by side.  The card stays hidden
    // until the QRZ wiring spots a station identifying itself in the RX
    // stream ("DE <call> <call>") and fills it with the lookup result.
    auto* cwTextRow = new QHBoxLayout;
    cwTextRow->setContentsMargins(0, 0, 0, 0);
    cwTextRow->setSpacing(4);
    cwTextRow->addWidget(m_cwText, 1);
    m_cwCallsignCard = new CallsignCard(CallsignCard::Variant::Compact, m_cwPanel);
    m_cwCallsignCard->setCloseButtonVisible(true);
    m_cwCallsignCard->setMinimumWidth(240);
    m_cwCallsignCard->setMaximumWidth(320);
    // Cap the height so the card stays card-shaped (not a full-height
    // sidebar) when the operator drags the decode panel tall.
    m_cwCallsignCard->setMaximumHeight(120);
    m_cwCallsignCard->setVisible(false);
    connect(m_cwCallsignCard, &CallsignCard::closeRequested,
            m_cwCallsignCard, &QWidget::hide);
    cwTextRow->addWidget(m_cwCallsignCard, 0, Qt::AlignTop);
    cwLayout->addLayout(cwTextRow);

    m_cwPanel->hide();
    layout->addWidget(m_cwPanel);

    // ── RTTY decode panel (hidden by default, shown in RTTY/DIGL mode) ───
    m_rttyPanel = new QWidget(this);
    m_rttyPanel->setCursor(Qt::ArrowCursor);
    m_rttyPanel->setFixedHeight(90);
    m_rttyPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_rttyPanel,
        "QWidget { background: {{color.background.0}}; border-top: 1px solid {{color.background.1}}; }");

    auto* rttyLayout = new QVBoxLayout(m_rttyPanel);
    rttyLayout->setContentsMargins(4, 2, 4, 2);
    rttyLayout->setSpacing(1);

    // ── Controls bar ─────────────────────────────────────────────────────
    auto* rttyBar = new QHBoxLayout;
    rttyBar->setSpacing(4);

    auto* rttyTitle = new QLabel("RTTY");
    AetherSDR::ThemeManager::instance().applyStyleSheet(rttyTitle,
        "QLabel { color: {{color.accent}}; font-size: 10px; font-weight: bold; background: transparent; }");
    rttyBar->addWidget(rttyTitle);

    const QString comboStyle =
        "QComboBox { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050;"
        " border-radius: 2px; font-size: 9px; padding: 0 2px; }"
        "QComboBox::drop-down { width: 10px; }"
        "QComboBox QAbstractItemView { background: #1a2a3a; color: #c8d8e8; selection-background-color: #304050; }";

    // Mark frequency
    auto* markLabel = new QLabel("Mark:");
    AetherSDR::ThemeManager::instance().applyStyleSheet(markLabel,
        "QLabel { color: {{color.text.label}}; font-size: 9px; background: transparent; }");
    rttyBar->addWidget(markLabel);

    m_rttyMarkCombo = new QComboBox;
    m_rttyMarkCombo->setStyleSheet(comboStyle);
    m_rttyMarkCombo->setFixedWidth(68);
    m_rttyMarkCombo->setToolTip("Mark (idle) tone audio frequency");
    // fldigi standard mark frequencies
    m_rttyMarkCombo->addItem("Auto",  0);
    m_rttyMarkCombo->addItem("2125",  2125);
    m_rttyMarkCombo->addItem("2210",  2210);
    m_rttyMarkCombo->addItem("1700",  1700);
    m_rttyMarkCombo->addItem("1275",  1275);
    m_rttyMarkCombo->addItem("1000",  1000);
    m_rttyMarkCombo->addItem("915",   915);
    m_rttyMarkCombo->addItem("850",   850);
    m_rttyMarkCombo->addItem("500",   500);
    {
        const int saved = AppSettings::instance().value("RttyDecoderMarkHz", "0").toInt();
        const int idx   = m_rttyMarkCombo->findData(saved);
        m_rttyMarkCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    connect(m_rttyMarkCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const int hz = m_rttyMarkCombo->currentData().toInt();
        AppSettings::instance().setValue("RttyDecoderMarkHz", QString::number(hz));
        AppSettings::instance().save();
        emit rttyMarkHzChanged(hz);
    });
    rttyBar->addWidget(m_rttyMarkCombo);

    // Shift
    auto* shiftLabel = new QLabel("Shift:");
    AetherSDR::ThemeManager::instance().applyStyleSheet(shiftLabel,
        "QLabel { color: {{color.text.label}}; font-size: 9px; background: transparent; }");
    rttyBar->addWidget(shiftLabel);

    m_rttyShiftCombo = new QComboBox;
    m_rttyShiftCombo->setStyleSheet(comboStyle);
    m_rttyShiftCombo->setFixedWidth(52);
    m_rttyShiftCombo->setToolTip("Mark-to-space frequency shift");
    // fldigi standard shifts
    for (int hz : {45, 50, 75, 100, 170, 182, 200, 240, 425, 450, 500, 850})
        m_rttyShiftCombo->addItem(QString::number(hz), hz);
    {
        const int saved = AppSettings::instance().value("RttyDecoderShiftHz", "170").toInt();
        const int idx   = m_rttyShiftCombo->findData(saved);
        m_rttyShiftCombo->setCurrentIndex(idx >= 0 ? idx : m_rttyShiftCombo->findData(170));
    }
    connect(m_rttyShiftCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const int hz = m_rttyShiftCombo->currentData().toInt();
        AppSettings::instance().setValue("RttyDecoderShiftHz", QString::number(hz));
        AppSettings::instance().save();
        emit rttyShiftHzChanged(hz);
    });
    rttyBar->addWidget(m_rttyShiftCombo);

    // Baud rate
    auto* baudLabel = new QLabel("Baud:");
    AetherSDR::ThemeManager::instance().applyStyleSheet(baudLabel,
        "QLabel { color: {{color.text.label}}; font-size: 9px; background: transparent; }");
    rttyBar->addWidget(baudLabel);

    m_rttyBaudCombo = new QComboBox;
    m_rttyBaudCombo->setStyleSheet(comboStyle);
    m_rttyBaudCombo->setFixedWidth(52);
    m_rttyBaudCombo->setToolTip("Symbol rate (baud)");
    // fldigi standard baud rates (stored as float * 100 to avoid float key issues)
    struct { const char* label; float val; } baudRates[] = {
        {"45.45", 45.45f}, {"50", 50.0f}, {"75", 75.0f}, {"100", 100.0f},
        {"110", 110.0f}, {"150", 150.0f}, {"300", 300.0f}
    };
    for (auto& b : baudRates)
        m_rttyBaudCombo->addItem(b.label, static_cast<double>(b.val));
    {
        const double saved = AppSettings::instance().value("RttyDecoderBaud", "45.45").toDouble();
        int bestIdx = 0;
        double bestDiff = 1e9;
        for (int i = 0; i < m_rttyBaudCombo->count(); ++i) {
            double diff = std::abs(m_rttyBaudCombo->itemData(i).toDouble() - saved);
            if (diff < bestDiff) { bestDiff = diff; bestIdx = i; }
        }
        m_rttyBaudCombo->setCurrentIndex(bestIdx);
    }
    connect(m_rttyBaudCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const float baud = static_cast<float>(m_rttyBaudCombo->currentData().toDouble());
        AppSettings::instance().setValue("RttyDecoderBaud", QString::number(static_cast<double>(baud)));
        AppSettings::instance().save();
        emit rttyBaudChanged(baud);
    });
    rttyBar->addWidget(m_rttyBaudCombo);

    // Rev toggle
    m_rttyRevBtn = new QPushButton("REV");
    m_rttyRevBtn->setCheckable(true);
    m_rttyRevBtn->setFixedSize(32, 16);
    m_rttyRevBtn->setToolTip("Reverse polarity: space above mark (LSB / inverted signal)");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_rttyRevBtn,
        "QPushButton { background: {{color.background.1}}; color: {{color.text.label}};"
        " border: 1px solid {{color.background.1}}; border-radius: 2px; font-size: 8px; padding: 0; }"
        "QPushButton:checked { color: {{color.accent}}; border-color: {{color.accent}}; }"
        "QPushButton:hover   { color: {{color.text.primary}}; }");
    {
        const bool saved = AppSettings::instance().value("RttyDecoderReverse", "false").toString() == "true";
        m_rttyRevBtn->setChecked(saved);
    }
    connect(m_rttyRevBtn, &QPushButton::toggled, this, [this](bool rev) {
        AppSettings::instance().setValue("RttyDecoderReverse", rev ? "true" : "false");
        AppSettings::instance().save();
        emit rttyReverseChanged(rev);
    });
    rttyBar->addWidget(m_rttyRevBtn);

    // Stats
    m_rttyStatsLabel = new QLabel;
    m_rttyStatsLabel->setTextFormat(Qt::RichText);
    m_rttyStatsLabel->setFixedWidth(248);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_rttyStatsLabel,
        "QLabel { background: transparent; }");
    rttyBar->addWidget(m_rttyStatsLabel);

    rttyBar->addStretch();

    const QString rttBtnStyle =
        "QPushButton { background: #1a2a3a; color: #8090a0; border: 1px solid #203040;"
        " border-radius: 2px; font-size: 9px; font-weight: bold; padding: 1px 6px; }"
        "QPushButton:hover { color: #c8d8e8; background: #2a3a4a; }";

    auto* rttyCopyAllBtn = new QPushButton("CPY ALL");
    rttyCopyAllBtn->setToolTip("Copy all decoded text to clipboard");
    rttyCopyAllBtn->setStyleSheet(rttBtnStyle);
    connect(rttyCopyAllBtn, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(m_rttyText->toPlainText());
    });
    rttyBar->addWidget(rttyCopyAllBtn);

    auto* clrBtn = new QPushButton("CLR");
    clrBtn->setStyleSheet(rttBtnStyle);
    connect(clrBtn, &QPushButton::clicked, this, &PanadapterApplet::clearRttyText);
    rttyBar->addWidget(clrBtn);

    auto* rttyCloseBtn = new QPushButton("✕");
    rttyCloseBtn->setToolTip("Close RTTY decoder");
    AetherSDR::ThemeManager::instance().applyStyleSheet(rttyCloseBtn,
        "QPushButton { background: {{color.background.1}}; color: {{color.text.secondary}};"
        " border: 1px solid {{color.background.1}}; border-radius: 2px; font-size: 9px;"
        " font-weight: bold; padding: 1px 6px; }"
        "QPushButton:hover { color: #ff6060; background: {{color.background.1}}; }");
    connect(rttyCloseBtn, &QPushButton::clicked, this, [this] {
        m_rttyPanel->hide();
        emit rttyPanelCloseRequested();
    });
    rttyBar->addWidget(rttyCloseBtn);

    rttyLayout->addLayout(rttyBar);

    // ── Text area ─────────────────────────────────────────────────────────
    m_rttyText = new QTextEdit;
    m_rttyText->setReadOnly(true);
    m_rttyText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_rttyText,
        "QTextEdit { background: {{color.background.0}}; color: {{color.accent.success}}; border: none;"
        " font-family: monospace; font-size: 13px; font-weight: bold; }"
        "QScrollBar:vertical { width: 6px; background: {{color.background.0}}; }"
        "QScrollBar::handle:vertical { background: {{color.background.2}}; border-radius: 3px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
    m_rttyText->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_rttyText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rttyText->setWordWrapMode(QTextOption::WrapAnywhere);
    m_rttyText->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_rttyText, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu* menu = m_rttyText->createStandardContextMenu();
        menu->addSeparator();
        menu->addAction(tr("Clear"), this, &PanadapterApplet::clearRttyText);
        menu->exec(m_rttyText->mapToGlobal(pos));
        delete menu;
    });
    rttyLayout->addWidget(m_rttyText);

    m_rttyPanel->hide();
    layout->addWidget(m_rttyPanel);
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

void PanadapterApplet::setSliceId(int id, const QString& perClientLetter)
{
    m_titleLabel->setText(
        QString("Slice %1").arg(SliceLabel::richText(id, perClientLetter)));
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

void PanadapterApplet::applyCwFont()
{
    // Re-apply the decoded-text stylesheet with the current font size.  The
    // inserted runs use colour-only <span>s, so the widget font governs them.
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_cwText,
        QStringLiteral(
            "QTextEdit { background: {{color.background.0}}; color: {{color.accent.success}}; border: none;"
            " font-family: monospace; font-size: %1px; font-weight: bold; }"
            "QScrollBar:vertical { width: 6px; background: {{color.background.0}}; }"
            "QScrollBar::handle:vertical { background: {{color.background.2}}; border-radius: 3px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
            .arg(m_cwFontPx));
}

void PanadapterApplet::adjustCwFont(int delta)
{
    const int next = std::clamp(m_cwFontPx + delta, 8, 32);
    if (next == m_cwFontPx)
        return;
    m_cwFontPx = next;
    applyCwFont();
    CwDecodeSettings::setFontPx(m_cwFontPx);
}

void PanadapterApplet::setCwPanelHeight(int h)
{
    // Live apply only — persistence happens once on grip release to avoid
    // writing settings on every mouse-move during a drag.
    m_cwPanelHeight = std::clamp(h, 60, 600);
    m_cwPanel->setFixedHeight(m_cwPanelHeight);
}

int PanadapterApplet::speedRangeLow()  const
{
    return m_speedRangeSlider ? m_speedRangeSlider->low() : 15;
}

int PanadapterApplet::speedRangeHigh() const
{
    return m_speedRangeSlider ? m_speedRangeSlider->high() : 40;
}

int PanadapterApplet::pitchRangeLow()  const
{
    return m_pitchRangeSlider ? m_pitchRangeSlider->low() : 500;
}

int PanadapterApplet::pitchRangeHigh() const
{
    return m_pitchRangeSlider ? m_pitchRangeSlider->high() : 700;
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
    // Switching back from TX → RX inserts a separator space so the [TX]
    // burst and the following RX text don't run together (#2417).
    if (m_lastCwTextSource == CwTextSource::Tx)
        m_cwText->insertHtml(QStringLiteral(" "));
    m_lastCwTextSource = CwTextSource::Rx;
    m_cwText->insertHtml(QString("<span style=\"color:%1\">%2</span>")
        .arg(color, clean.toHtmlEscaped()));
    m_cwText->moveCursor(QTextCursor::End);

    emit cwRxTextDisplayed(clean);
}

void PanadapterApplet::appendCwTextTx(const QString& text, float cost)
{
    // TX-side decoded keying (#2417).  Same confidence filter as the RX
    // path; rendered in a distinct cyan color so the operator can tell
    // their own sending apart from incoming CW when both directions feed
    // the same panel.  No textual marker — color alone is the signal.
    if (cost >= m_cwCostThreshold) return;

    QString clean = text;
    clean.replace('\n', ' ');
    if (clean.trimmed().isEmpty()) return;

    constexpr const char* kTxColor = "#5fc8ff";
    m_cwText->moveCursor(QTextCursor::End);
    // Separator space on Rx→Tx boundary so the two colored runs don't
    // visually merge.  None→Tx (panel empty) gets no leading space.
    if (m_lastCwTextSource == CwTextSource::Rx)
        m_cwText->insertHtml(QStringLiteral(" "));
    m_lastCwTextSource = CwTextSource::Tx;
    m_cwText->insertHtml(
        QString("<span style=\"color:%1\">%2</span>")
            .arg(QLatin1String(kTxColor), clean.toHtmlEscaped()));
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
    m_lastCwTextSource = CwTextSource::None;
}

bool PanadapterApplet::eventFilter(QObject* obj, QEvent* ev)
{
    // CW panel resize grip — drag the top edge to grow/shrink the decode
    // panel (and reveal more decoded-text history) (#3628).
    if (obj == m_cwGrip) {
        if (ev->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) {
                m_cwResizing     = true;
                m_cwResizeStartY = me->globalPosition().toPoint().y();
                m_cwResizeStartH = m_cwPanel->height();
                return true;
            }
        } else if (ev->type() == QEvent::MouseMove && m_cwResizing) {
            auto* me = static_cast<QMouseEvent*>(ev);
            // Dragging up (smaller Y) enlarges the panel.
            const int dy = m_cwResizeStartY - me->globalPosition().toPoint().y();
            setCwPanelHeight(m_cwResizeStartH + dy);
            return true;
        } else if (ev->type() == QEvent::MouseButtonRelease && m_cwResizing) {
            m_cwResizing = false;
            CwDecodeSettings::setPanelHeight(m_cwPanelHeight);
            return true;
        }
    }

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

// ── RTTY panel ───────────────────────────────────────────────────────────────

void PanadapterApplet::setRttyPanelVisible(bool visible)
{
    m_rttyPanel->setVisible(visible);
}

int PanadapterApplet::rttyMarkHz() const
{
    return m_rttyMarkCombo->currentData().toInt();
}

int PanadapterApplet::rttyShiftHz() const
{
    return m_rttyShiftCombo->currentData().toInt();
}

float PanadapterApplet::rttyBaud() const
{
    return static_cast<float>(m_rttyBaudCombo->currentData().toDouble());
}

bool PanadapterApplet::rttyReverse() const
{
    return m_rttyRevBtn->isChecked();
}

void PanadapterApplet::appendRttyText(const QString& text, float confidence)
{
    // CR is a no-op in a wrapped text view; LF becomes a line break.
    // Standard RTTY sends CR+LF pairs — discarding CR and converting LF
    // to <br> produces exactly one new line per pair.
    if (text == "\r") return;
    if (text == "\n") {
        m_rttyText->moveCursor(QTextCursor::End);
        m_rttyText->insertHtml(QStringLiteral("<br>"));
        m_rttyText->moveCursor(QTextCursor::End);
        return;
    }

    QString color;
    if      (confidence > 0.85f) color = "#00ff88";
    else if (confidence > 0.70f) color = "#e0e040";
    else if (confidence > 0.60f) color = "#ff9020";
    else                         color = "#ff4040";

    QString escaped = text.toHtmlEscaped();
    escaped.replace(' ', "&nbsp;");

    m_rttyText->moveCursor(QTextCursor::End);
    m_rttyText->insertHtml(QString("<span style=\"color:%1\">%2</span>")
        .arg(color, escaped));
    m_rttyText->moveCursor(QTextCursor::End);
}

void PanadapterApplet::setRttyStats(float markLevel, float /* spaceLevel */, float snrDb, bool locked)
{
    const int markSegs = qBound(0, qRound(markLevel * 15.0f), 15);

    // Mark bar: 15 segments, fills left to right
    QString markBar;
    for (int i = 0; i < 15; ++i) {
        markBar += (i < markSegs)
            ? QStringLiteral("<span style='color:#00cc55'>&#x2588;</span>")
            : QStringLiteral("<span style='color:#1a3020'>&#x2588;</span>");
    }

    const QString snrColor   = locked ? QStringLiteral("#00ff88") : QStringLiteral("#ff8c00");
    const QString lockedText = locked ? QStringLiteral("LOCKED") : QStringLiteral("UNLOCK");

    m_rttyStatsLabel->setText(
        QStringLiteral("<span style='font-family:monospace;font-size:10px;'>")
        + markBar
        + QStringLiteral("</span>&nbsp;&nbsp;"
                          "<span style='font-size:10px;font-weight:bold;color:")
        + snrColor + QStringLiteral("'>")
        + QString::number(static_cast<int>(snrDb))
        + QStringLiteral("dB&nbsp;")
        + lockedText
        + QStringLiteral("</span>"));
}

void PanadapterApplet::clearRttyText()
{
    m_rttyText->clear();
}

} // namespace AetherSDR
