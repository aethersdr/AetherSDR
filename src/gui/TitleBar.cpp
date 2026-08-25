#include "TitleBar.h"
#include "BrandMark.h"
#include "FramelessMessageBox.h"
#include "FramelessMoveHelper.h"
#include "GuardedSlider.h"
#include "PersistentDialog.h"
#include "RadioTabBar.h"
#include "WindowCaptionButtons.h"
#include "core/AppSettings.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QVBoxLayout>
#include <QDialog>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QDesktopServices>
#include <QClipboard>
#include <QApplication>
#include <QTimer>
#include <QAbstractAnimation>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QWindow>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include "core/VersionNumber.h"
#include "core/ThemeManager.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AetherSDR {

namespace {
constexpr const char* kTitleDragHandleProperty = "aetherTitleDragHandle";

// Stall timeout for the GitHub latest-release version check (#4688 §6).
constexpr int kTransferTimeoutMs = 15000;

// Build a 16×18 dock-side indicator: hollow rectangle (the main window)
// with a thin shaded strip flush against one inner wall, representing
// the applet panel docked on that side.  Visual language matches the
// pop-out icon's thin-strip-as-panel motif.
QPixmap buildDockSideIcon(const QWidget* widget, bool fillLeft, bool active)
{
    QPixmap pm(16, 18);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QColor stroke = ThemeManager::instance().color(
        widget, active ? QStringLiteral("color.titlebar.caption.glyph.hover")
                       : QStringLiteral("color.titlebar.caption.glyph"));
    p.setPen(QPen(stroke, 1));
    p.setBrush(Qt::NoBrush);
    // Outline 13×14: columns 1 and 14, rows 2 and 16.  Interior 12×13.
    p.drawRect(1, 2, 13, 14);
    // Shaded strip 4 px wide, spanning top-to-bottom of the outer rect
    // (rows 3–15 = full interior height), flush against the chosen wall:
    //   left  = cols 2–5
    //   right = cols 10–13
    if (fillLeft)
        p.fillRect(2,  3, 4, 13, stroke);
    else
        p.fillRect(10, 3, 4, 13, stroke);
    p.end();
    return pm;
}

// Build a 16×18 pop-out indicator: hollow square (the main waterfall
// window) on the left, with a smaller filled rectangle to its right
// representing the applet panel detached into its own window.  The
// filled area is 25% of the hollow square's interior so the relative
// sizing reads as "small floating window".
QPixmap buildPopOutIcon(const QWidget* widget, bool active)
{
    QPixmap pm(16, 18);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QColor stroke = ThemeManager::instance().color(
        widget, active ? QStringLiteral("color.titlebar.caption.glyph.hover")
                       : QStringLiteral("color.titlebar.caption.glyph"));
    p.setPen(QPen(stroke, 1));
    p.setBrush(Qt::NoBrush);
    // Main-window hollow rect on the left.  Same vertical extent as the
    // dock-side icons (rows 2–16, 14 tall outline) so all three icons line
    // up; horizontally narrower since the popped-out strip lives outside.
    p.drawRect(1, 2, 8, 14);
    // Popped-out applet — 4 px wide strip spanning the full top-to-bottom
    // height of the outer hollow rect (rows 3–15).  Positioned outside the
    // hollow with a 2 px gap (columns 10 and 11) so the detachment reads.
    p.fillRect(12, 3, 4, 13, stroke);
    p.end();
    return pm;
}

// ── Audio-cluster icons ─────────────────────────────────────────────────────
// Thin-line marks in the house style (1.8 px stroke, no fill), replacing the
// 🔊 / 🎧 emoji the strip used to carry.  The design system forbids emoji as UI
// outright: they render in the platform's own colour and weight, so they never
// matched the bar around them and never followed the theme.
enum class AudioIcon { Speaker, Headphone };

QPixmap buildAudioIcon(AudioIcon kind, bool muted, const QColor& stroke, qreal dpr)
{
    constexpr int kBox = 20;
    QPixmap pm(int(kBox * dpr), int(kBox * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(stroke, 1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    if (kind == AudioIcon::Speaker) {
        // Cone: a small rectangle at the pivot with a flared face.
        QPainterPath cone;
        cone.moveTo(3.0, 8.0);
        cone.lineTo(6.5, 8.0);
        cone.lineTo(10.5, 4.5);
        cone.lineTo(10.5, 15.5);
        cone.lineTo(6.5, 12.0);
        cone.lineTo(3.0, 12.0);
        cone.closeSubpath();
        p.drawPath(cone);
        if (!muted) {
            // Two radiating arcs.  QPainter angles are sixteenths of a degree.
            p.drawArc(QRectF(9.0, 5.5, 6.0, 9.0), -60 * 16, 120 * 16);
            p.drawArc(QRectF(9.0, 2.5, 9.0, 15.0), -55 * 16, 110 * 16);
        } else {
            p.drawLine(QPointF(13.0, 7.0), QPointF(17.5, 13.0));
            p.drawLine(QPointF(17.5, 7.0), QPointF(13.0, 13.0));
        }
    } else {
        // Headband arc plus two ear cups.
        p.drawArc(QRectF(3.5, 3.5, 13.0, 13.0), 0, 180 * 16);
        p.drawRoundedRect(QRectF(3.0, 10.0, 3.6, 6.5), 1.6, 1.6);
        p.drawRoundedRect(QRectF(13.4, 10.0, 3.6, 6.5), 1.6, 1.6);
        if (muted) {
            p.drawLine(QPointF(3.5, 16.5), QPointF(16.5, 3.5));
        }
    }
    p.end();
    return pm;
}

// Set (or re-set) an audio button's icon at the current DPR and theme colour.
// Called on construction, on every mute toggle, and on theme change.
void applyAudioIcon(QPushButton* button, AudioIcon kind, bool muted)
{
    if (!button) {
        return;
    }
    const QColor stroke =
        AetherSDR::ThemeManager::instance().color(button,
                                                  QStringLiteral("color.text.secondary"));
    button->setIcon(QIcon(buildAudioIcon(kind, muted, stroke,
                                         button->devicePixelRatioF())));
    button->setIconSize(QSize(20, 20));
}
}

TitleBar::TitleBar(QWidget* parent)
    : QWidget(parent)
{
    AetherSDR::theme::setContainer(this, QStringLiteral("titlebar"));
    setFixedHeight(kHeight);
    applyBarStyle();

    m_hbox = new QHBoxLayout(this);
    m_hbox->setContentsMargins(16, 0, 16, 0);
    m_hbox->setSpacing(16);

    auto makeDragGutter = [this]() {
        auto* gutter = new QWidget(this);
        gutter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        gutter->setMinimumWidth(0);
        markDragHandle(gutter);
        return gutter;
    };

    // Keep the identity/status cluster anchored on the left, immediately after
    // the menu bar once it is inserted via setMenuBar().

    // ── Radio-link (discovery heartbeat) state ──────────────────────────────
    // There is no separate heartbeat lamp any more: the active radio tab's own
    // status dot carries it.  One dot answers both "which radio is this" and
    // "is its link alive", which is where the operator is already looking — and
    // it retires an indicator whose meaning had to be learned from a tooltip.
    //
    // The state machine below is unchanged; only its output moved.  That
    // matters because two of its rules are safety-critical and easy to lose in
    // a re-implementation: three missed beats is the alarm threshold, and the
    // alarm holds SOLID red when blinking is disabled rather than going quiet.

    // Load persisted blink preference (default: enabled)
    m_blinkEnabled = AppSettings::instance()
        .value("HeartbeatBlinkEnabled", "True").toString() == "True";

    // ── 1. Window controls ──────────────────────────────────────────────────
    // The window is frameless on every platform, so the controls are always
    // ours.  macOS and Linux put them at the left of the bar (traffic lights
    // and chips respectively); Windows puts its caption buttons at the far
    // right instead, so nothing goes in this slot there.
#if defined(Q_OS_MAC)
    m_captionButtons = new WindowCaptionButtons(CaptionStyle::MacTrafficLights, this);
    m_hbox->addWidget(m_captionButtons);
    // The platform sets its lights ~20 px in from the window edge; the layout's
    // own 16 px margin covers most of that, so add the remainder as spacing
    // rather than special-casing the margin.
    m_hbox->addSpacing(4);
#elif !defined(Q_OS_WIN)
    m_captionButtons = new WindowCaptionButtons(CaptionStyle::LinuxChips, this);
    m_hbox->addWidget(m_captionButtons);
#endif

    // ── 2. Brand ────────────────────────────────────────────────────────────
    m_brand = new BrandMark(this);
    markDragHandle(m_brand);
    m_hbox->addWidget(m_brand);

    // The menu bar (Linux/Windows only — macOS puts it in the system bar) is
    // inserted here by setMenuBar(), between the brand and the radio tabs.
    m_menuBarSlot = m_hbox->count();

    // ── 3. Radio tabs ───────────────────────────────────────────────────────
    m_radioTabs = new RadioTabBar(this);
    m_radioTabs->setPulseEnabled(m_blinkEnabled);
    connect(m_radioTabs, &RadioTabBar::radioActivated,
            this, &TitleBar::radioTabActivated);
    connect(m_radioTabs, &RadioTabBar::connectManuallyRequested,
            this, &TitleBar::connectManuallyRequested);
    // Right-click a tab to toggle the blink, as the old heartbeat lamp did.
    // The View menu keeps its checkbox; both route through setBlinkEnabled().
    m_radioTabs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_radioTabs, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        // Heap-allocate with WA_DeleteOnClose so the menu outlives this lambda.
        // popup() not exec(): exec() spins a nested event loop, which can let
        // network/connection events run out of order while the menu is open.
        QMenu* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        QAction* blinkAction = menu->addAction("Blink status indicator");
        blinkAction->setCheckable(true);
        blinkAction->setChecked(m_blinkEnabled);
        connect(blinkAction, &QAction::triggered, this, [this](bool checked) {
            setBlinkEnabled(checked);
        });
        menu->popup(m_radioTabs->mapToGlobal(pos));
    });
    m_hbox->addWidget(m_radioTabs);

    m_experimentalRadioLabel = new QLabel(QStringLiteral("EXPERIMENTAL"));
    m_experimentalRadioLabel->setObjectName(QStringLiteral("experimentalRadioBadge"));
    m_experimentalRadioLabel->setFixedHeight(20);
    m_experimentalRadioLabel->setAlignment(Qt::AlignCenter);
    AetherSDR::ThemeManager::instance().applyStyleSheet(
        m_experimentalRadioLabel,
        "QLabel { color: {{color.accent.warning}}; border: 1px solid "
        "{{color.accent.warning}}; border-radius: 3px; background: transparent; "
        "font-size: 9px; font-weight: bold; padding: 0px 5px; }");
    m_experimentalRadioLabel->setAccessibleName(
        QStringLiteral("Experimental radio support"));
    markDragHandle(m_experimentalRadioLabel);
    m_experimentalRadioLabel->hide();
    m_hbox->addWidget(m_experimentalRadioLabel);

    // multiFLEX rides alongside the tabs: it describes the same radio session
    // the active tab names.
    m_mfBtn = new QPushButton("multiFLEX");
    m_mfBtn->setFlat(true);
    ThemeManager::instance().applyStyleSheet(
        m_mfBtn,
        QStringLiteral(
            "QPushButton { color: {{color.accent.success}}; font-size: 11px;"
            " font-weight: bold; border: 1px solid {{color.accent.success}};"
            " border-radius: 4px; background: transparent; padding: 0px 3px; }"
            "QPushButton:hover { background: {{color.titlebar.tab.active.background}}; }"));
    m_mfBtn->setVisible(false);
    m_mfBtn->setCursor(Qt::PointingHandCursor);
    m_mfBtn->setAccessibleName("multiFLEX status");
    connect(m_mfBtn, &QPushButton::clicked, this, &TitleBar::multiFlexClicked);
    m_hbox->addWidget(m_mfBtn);

    // ── 4. Spacer ───────────────────────────────────────────────────────────
    m_hbox->addWidget(makeDragGutter(), 1);

    // ── 5. Audio cluster ────────────────────────────────────────────────────
    // Grouped into its own container so the bar's 16 px inter-group gap applies
    // between the cluster and the spacer, while the controls *inside* it keep
    // the tighter 8 px rhythm the instrument UI uses everywhere else.
    auto* audioCluster = new QWidget(this);
    audioCluster->setObjectName(QStringLiteral("titleBarAudioCluster"));
    audioCluster->setAccessibleName(QStringLiteral("Audio controls"));
    auto* audio = new QHBoxLayout(audioCluster);
    audio->setContentsMargins(0, 0, 0, 0);
    audio->setSpacing(8);

    m_otherTxLabel = new QLabel();
    ThemeManager::instance().applyStyleSheet(
        m_otherTxLabel,
        QStringLiteral(
            "QLabel { background: {{color.titlebar.caption.close.glyph}};"
            " color: {{color.accent.danger}}; font-size: 12px; font-weight: bold;"
            " border-radius: 3px; padding: 2px 8px; }"));
    m_otherTxLabel->setVisible(false);
    markDragHandle(m_otherTxLabel);
    audio->addWidget(m_otherTxLabel);

    // ── PC Audio + Master Vol + HP Vol ──────────────────────────────────────
    auto& s = AppSettings::instance();

    // ── Transmit timer ──────────────────────────────────────────────────────
    // (The Pan Lock button that used to sit here was removed with #4116's
    // per-pan Center Lock.)
    // Bright-green (network-status green), square-outlined elapsed timer that
    // runs while the operator is keyed (MOX/PTT/VOX — never TCI/DAX). Hidden
    // when idle; setOperatorTransmitting() drives show/hold/fade. Sits to the
    // left of the PC Audio button.
    m_txTimerLabel = new QLabel(QStringLiteral("0:00"));
    m_txTimerLabel->setObjectName(QStringLiteral("txTimerLabel"));
    m_txTimerLabel->setFixedHeight(22);
    m_txTimerLabel->setMinimumWidth(52);
    m_txTimerLabel->setAlignment(Qt::AlignCenter);
    ThemeManager::instance().applyStyleSheet(
        m_txTimerLabel,
        QStringLiteral(
            "QLabel { color: {{color.accent.success}};"
            " border: 1px solid {{color.accent.success}}; border-radius: 3px;"
            " background: transparent; font-size: 11px; font-weight: bold;"
            " padding: 0px 4px; }"));
    m_txTimerLabel->setToolTip(QStringLiteral("Transmit timer — elapsed key-down time"));
    m_txTimerLabel->setAccessibleName(QStringLiteral("Transmit timer"));
    m_txTimerLabel->setAccessibleDescription(
        QStringLiteral("Elapsed transmit time while keyed (MOX/PTT/VOX)"));
    markDragHandle(m_txTimerLabel);
    // Opacity effect powers the 15s-hold → fade-out on unkey.
    m_txTimerOpacity = new QGraphicsOpacityEffect(m_txTimerLabel);
    m_txTimerOpacity->setOpacity(1.0);
    m_txTimerLabel->setGraphicsEffect(m_txTimerOpacity);
    m_txTimerLabel->setVisible(false);   // idle: not displayed
    audio->addWidget(m_txTimerLabel);

    // 5 Hz tick that repaints the elapsed time while keyed.
    m_txTimerTick = new QTimer(this);
    m_txTimerTick->setInterval(200);   // 5Hz so the displayed second tracks the
                                       // true elapsed time within ~200ms
    connect(m_txTimerTick, &QTimer::timeout, this, [this]() { updateTxTimerText(); });

    // 15s post-unkey hold, then fade the label out.
    m_txTimerHoldTimer = new QTimer(this);
    m_txTimerHoldTimer->setSingleShot(true);
    m_txTimerHoldTimer->setInterval(15'000);
    connect(m_txTimerHoldTimer, &QTimer::timeout, this, [this]() {
        m_txTimerFade->stop();
        m_txTimerOpacity->setOpacity(1.0);
        m_txTimerFade->setStartValue(1.0);
        m_txTimerFade->setEndValue(0.0);
        m_txTimerFade->start();
    });

    // Fade-out animation; hides the label once fully transparent.
    m_txTimerFade = new QPropertyAnimation(m_txTimerOpacity, "opacity", this);
    m_txTimerFade->setDuration(800);
    connect(m_txTimerFade, &QPropertyAnimation::finished, this, [this]() {
        if (m_txTimerOpacity->opacity() <= 0.01)
            m_txTimerLabel->setVisible(false);
    });

    // PC Audio pill
    m_pcBtn = new QPushButton("PC Audio");
    m_pcBtn->setObjectName(QStringLiteral("pcAudioBtn"));
    m_pcBtn->setCheckable(true);
    m_pcBtn->setFixedHeight(24);
    m_pcBtn->setFixedWidth(78);
    m_pcBtn->setCursor(Qt::PointingHandCursor);

    bool pcOn = s.value("PcAudioEnabled", "True").toString() == "True";
    m_pcBtn->setChecked(pcOn);
    m_pcBtn->setAccessibleName("PC Audio");
    m_pcBtn->setAccessibleDescription(
        "Toggle PC receive playback and PC microphone voice transmit");
    updatePcAudioToolTip();

    applyPcAudioStyle();

    connect(m_pcBtn, &QPushButton::toggled, this, [this](bool on) {
        applyPcAudioStyle();
        auto& ss = AppSettings::instance();
        ss.setValue("PcAudioEnabled", on ? "True" : "False");
        ss.save();
        emit pcAudioToggled(on);
    });
    audio->addWidget(m_pcBtn);

    // 64 px track, 3 px groove, 10 px knob — the bar's own slider metrics,
    // narrower and thinner than the app-wide default so two of them fit next to
    // the tabs without crowding.
    const QString barSliderStyle = QStringLiteral(
        "QSlider::groove:horizontal { background: {{color.slider.background}};"
        " height: 3px; border-radius: 1px; }"
        "QSlider::handle:horizontal { background: {{color.slider.handle}};"
        " width: 10px; height: 10px; margin: -4px 0; border-radius: 5px; }"
        "QSlider::sub-page:horizontal { background: {{color.accent}};"
        " border-radius: 1px; }");
    // JetBrains Mono for the numeric readouts — the house mono face for any
    // value the operator reads rather than reads *about*.
    const QString barValueStyle = QStringLiteral(
        "QLabel { color: {{color.text.secondary}}; font-size: 11px;"
        " font-family: \"{{font.family.mono}}\", \"JetBrains Mono\", monospace; }");
    const QString iconBtnStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none; padding: 0;"
        " border-radius: 4px; }"
        "QPushButton:hover { background: {{color.titlebar.tab.hover}}; }"
        "QPushButton:focus { border: 1px solid {{color.border.accent}}; }");

    // Line-out volume (click icon to mute/unmute)
    m_speakerBtn = new QPushButton;
    m_speakerBtn->setFixedSize(22, 22);
    m_speakerBtn->setCheckable(true);
    m_speakerBtn->setCursor(Qt::PointingHandCursor);
    // Tab-only focus: a click mutes, it does not leave the accent ring
    // painted around the icon afterwards.  The ring is kept for keyboard
    // navigation, where it is the only thing showing where focus sits.
    m_speakerBtn->setFocusPolicy(Qt::TabFocus);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_speakerBtn, iconBtnStyle);
    m_speakerBtn->setToolTip("Click to mute/unmute line out");
    m_speakerBtn->setAccessibleName("Line out mute");
    m_speakerBtn->setAccessibleDescription("Mute or unmute line out speaker audio");
    applyAudioIcon(m_speakerBtn, AudioIcon::Speaker, false);
    connect(m_speakerBtn, &QPushButton::toggled, this, [this](bool muted) {
        applyAudioIcon(m_speakerBtn, AudioIcon::Speaker, muted);
        emit lineoutMuteChanged(muted);
    });
    audio->addWidget(m_speakerBtn);

    m_masterSlider = new GuardedSlider(Qt::Horizontal);
    m_masterSlider->setRange(0, 100);
    int savedVol = s.value("MasterVolume", "100").toInt();
    m_masterSlider->setValue(savedVol);
    m_masterSlider->setFixedWidth(64);
    m_masterSlider->setFixedHeight(16);
    m_masterSlider->setAccessibleName("Master volume");
    m_masterSlider->setAccessibleDescription("Line out volume level, 0 to 100 percent");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_masterSlider, barSliderStyle);
    audio->addWidget(m_masterSlider);

    m_masterLabel = new QLabel(QString::number(savedVol));
    m_masterLabel->setFixedWidth(24);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_masterLabel, barValueStyle);
    m_masterLabel->setAlignment(Qt::AlignCenter);
    markDragHandle(m_masterLabel);
    audio->addWidget(m_masterLabel);

    connect(m_masterSlider, &QSlider::valueChanged, this, [this](int v) {
        m_masterLabel->setText(QString::number(v));
        emit masterVolumeChanged(v);
    });

    // Headphone volume (click icon to mute/unmute)
    m_headphoneBtn = new QPushButton;
    m_headphoneBtn->setFixedSize(22, 22);
    m_headphoneBtn->setCheckable(true);
    m_headphoneBtn->setCursor(Qt::PointingHandCursor);
    // Tab-only focus — see the line-out button above.
    m_headphoneBtn->setFocusPolicy(Qt::TabFocus);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_headphoneBtn, iconBtnStyle);
    m_headphoneBtn->setToolTip("Click to mute/unmute headphones");
    m_headphoneBtn->setAccessibleName("Headphone mute");
    m_headphoneBtn->setAccessibleDescription("Mute or unmute headphone audio");
    applyAudioIcon(m_headphoneBtn, AudioIcon::Headphone, false);
    connect(m_headphoneBtn, &QPushButton::toggled, this, [this](bool muted) {
        applyAudioIcon(m_headphoneBtn, AudioIcon::Headphone, muted);
        emit headphoneMuteChanged(muted);
    });
    audio->addWidget(m_headphoneBtn);

    m_hpSlider = new GuardedSlider(Qt::Horizontal);
    m_hpSlider->setRange(0, 100);
    m_hpSlider->setValue(50);
    m_hpSlider->setFixedWidth(64);
    m_hpSlider->setFixedHeight(16);
    m_hpSlider->setAccessibleName("Headphone volume");
    m_hpSlider->setAccessibleDescription("Headphone volume level, 0 to 100 percent");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_hpSlider, barSliderStyle);
    audio->addWidget(m_hpSlider);

    m_hpLabel = new QLabel("50");
    m_hpLabel->setFixedWidth(24);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_hpLabel, barValueStyle);
    m_hpLabel->setAlignment(Qt::AlignCenter);
    markDragHandle(m_hpLabel);
    audio->addWidget(m_hpLabel);

    connect(m_hpSlider, &QSlider::valueChanged, this, [this](int v) {
        m_hpLabel->setText(QString::number(v));
        emit headphoneVolumeChanged(v);
    });

    m_hbox->addWidget(audioCluster);

    // ── Dock-side selectors + our own caption controls ──────────────────────
    auto* sep = new QFrame;
    sep->setFixedSize(1, 20);
    AetherSDR::ThemeManager::instance().applyStyleSheet(sep, "QFrame { background: {{color.background.2}}; border: none; }");
    markDragHandle(sep);
    m_hbox->addWidget(sep);

    const QString dockLblStyle = QStringLiteral(
        "QLabel { padding: 0 6px; border-radius: 4px; }"
        "QLabel:hover { background: {{color.titlebar.caption.hover}}; }");

    // Dock-side selectors (applet panel left vs right of the panadapter).
    // Click is wired via eventFilter() like the min/max/close trio.
    m_dockLeftLbl = new QLabel;
    m_dockLeftLbl->setObjectName(QStringLiteral("appletDockLeftBtn"));
    m_dockLeftLbl->setFixedHeight(24);
    m_dockLeftLbl->setAlignment(Qt::AlignCenter);
    m_dockLeftLbl->setCursor(Qt::PointingHandCursor);
    m_dockLeftLbl->setToolTip("Dock applet panel to the left of the panadapter");
    m_dockLeftLbl->setAccessibleName(tr("Dock applet panel left"));
    m_dockLeftLbl->setAccessibleDescription(
        tr("Move the applet panel to the left of the panadapter, or hide it "
           "if it is already docked there"));
    ThemeManager::instance().applyStyleSheet(m_dockLeftLbl, dockLblStyle);
    m_dockLeftLbl->setPixmap(
        buildDockSideIcon(m_dockLeftLbl, /*fillLeft=*/true, /*active=*/false));
    m_dockLeftLbl->installEventFilter(this);
    m_hbox->addWidget(m_dockLeftLbl);

    m_dockRightLbl = new QLabel;
    m_dockRightLbl->setObjectName(QStringLiteral("appletDockRightBtn"));
    m_dockRightLbl->setFixedHeight(24);
    m_dockRightLbl->setAlignment(Qt::AlignCenter);
    m_dockRightLbl->setCursor(Qt::PointingHandCursor);
    m_dockRightLbl->setToolTip("Dock applet panel to the right of the panadapter");
    m_dockRightLbl->setAccessibleName(tr("Dock applet panel right"));
    m_dockRightLbl->setAccessibleDescription(
        tr("Move the applet panel to the right of the panadapter, or hide it "
           "if it is already docked there"));
    ThemeManager::instance().applyStyleSheet(m_dockRightLbl, dockLblStyle);
    m_dockRightLbl->setPixmap(
        buildDockSideIcon(m_dockRightLbl, /*fillLeft=*/false, /*active=*/true));
    m_dockRightLbl->installEventFilter(this);
    m_hbox->addWidget(m_dockRightLbl);

    m_popOutLbl = new QLabel;
    m_popOutLbl->setObjectName(QStringLiteral("appletPopOutBtn"));
    m_popOutLbl->setFixedHeight(24);
    m_popOutLbl->setAlignment(Qt::AlignCenter);
    m_popOutLbl->setCursor(Qt::PointingHandCursor);
    m_popOutLbl->setToolTip("Pop the applet panel out into its own window");
    m_popOutLbl->setAccessibleName(tr("Pop out applet panel"));
    m_popOutLbl->setAccessibleDescription(
        tr("Float the applet panel in its own window, or dock it back into "
           "the main window"));
    ThemeManager::instance().applyStyleSheet(m_popOutLbl, dockLblStyle);
    m_popOutLbl->setPixmap(buildPopOutIcon(m_popOutLbl, /*active=*/false));
    m_popOutLbl->installEventFilter(this);
    m_hbox->addWidget(m_popOutLbl);

#if defined(Q_OS_WIN)
    // Trailing separator only where something follows it.  Windows keeps its
    // caption buttons at the right, so the rule divides the pane controls from
    // them; on macOS and Linux the controls are at the far left and this rule
    // would just hang off the end of the row with nothing to separate.
    m_dockSep = new QFrame;
    m_dockSep->setFixedSize(1, 20);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_dockSep, "QFrame { background: {{color.background.2}}; border: none; }");
    markDragHandle(m_dockSep);
    m_hbox->addWidget(m_dockSep);


    // Design 1e: the caption cluster is flush to the right window edge and runs
    // the full 52 px, so it cancels the layout's right margin and gap.
    m_captionButtons = new WindowCaptionButtons(CaptionStyle::WindowsCaption, this);
    m_hbox->addWidget(m_captionButtons);
    m_hbox->setContentsMargins(16, 0, 0, 0);
    m_hbox->setStretchFactor(m_captionButtons, 0);
#endif

    if (m_captionButtons) {
        connect(m_captionButtons, &WindowCaptionButtons::minimizeRequested,
                this, [this]() {
                    if (auto* w = window()) w->showMinimized();
                });
        connect(m_captionButtons, &WindowCaptionButtons::maximizeRestoreRequested,
                this, [this]() {
                    if (m_minimalMode) {
                        emit minimalModeWindowedExitRequested();
                        return;
                    }
                    if (auto* w = window()) {
                        if (w->isMaximized()) w->showNormal();
                        else                  w->showMaximized();
                    }
                });
        connect(m_captionButtons, &WindowCaptionButtons::closeRequested,
                this, [this]() {
                    if (auto* w = window()) w->close();
                });
    }

    // Painter-drawn icons don't ride ThemeManager's stylesheet re-apply, so
    // repaint them explicitly when the palette changes.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyBarStyle();
        applyAudioIcon(m_speakerBtn, AudioIcon::Speaker, m_speakerBtn->isChecked());
        applyAudioIcon(m_headphoneBtn, AudioIcon::Headphone, m_headphoneBtn->isChecked());
        setAppletDockState(m_appletPanelVisible, m_appletPanelDockedLeft);
        setAppletFloating(m_appletPanelFloating);
    });
}

void TitleBar::applyBarStyle()
{
#ifdef Q_OS_MAC
    const QString backgroundToken = QStringLiteral("color.titlebar.background.mac");
#else
    const QString backgroundToken = QStringLiteral("color.titlebar.background");
#endif
    AetherSDR::ThemeManager::instance().applyStyleSheet(
        this,
        QStringLiteral("TitleBar { background: {{%1}};"
                       " border-bottom: 1px solid {{color.titlebar.border}}; }")
            .arg(backgroundToken));
}

void TitleBar::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    // Install event filter on the host window once it's known so we can
    // refresh the maximize-button icon when the window state changes.
    if (auto* w = window()) {
        w->installEventFilter(this);
        updateMaximizeIcon();
    }
}

void TitleBar::setRadioTabs(const QList<RadioTabEntry>& radios)
{
    if (m_radioTabs) {
        m_radioTabs->setRadios(radios);
    }
}

void TitleBar::setActiveRadio(const QString& id)
{
    if (m_radioTabs) {
        m_radioTabs->setActiveRadio(id);
    }
}

void TitleBar::setDiscoveredRadios(const QList<RadioTabEntry>& radios)
{
    if (m_radioTabs) {
        m_radioTabs->setDiscoveredRadios(radios);
    }
}

QVariantMap TitleBar::barState() const
{
    QVariantMap audio{
        {QStringLiteral("pcAudioEnabled"), m_pcBtn && m_pcBtn->isChecked()},
        {QStringLiteral("pcAudioLocked"), m_pcBtn && !m_pcBtn->isEnabled()},
        {QStringLiteral("lineoutMuted"), m_speakerBtn && m_speakerBtn->isChecked()},
        {QStringLiteral("headphoneMuted"), m_headphoneBtn && m_headphoneBtn->isChecked()},
        {QStringLiteral("masterVolume"), m_masterSlider ? m_masterSlider->value() : 0},
        {QStringLiteral("headphoneVolume"), m_hpSlider ? m_hpSlider->value() : 0},
        {QStringLiteral("masterText"), m_masterLabel ? m_masterLabel->text() : QString()},
        {QStringLiteral("headphoneText"), m_hpLabel ? m_hpLabel->text() : QString()},
        {QStringLiteral("sliderWidth"), m_masterSlider ? m_masterSlider->width() : 0},
    };

    QVariantMap chrome{
        {QStringLiteral("frameless"),
         window() && window()->windowFlags().testFlag(Qt::FramelessWindowHint)},
    };
    if (m_captionButtons) {
        chrome.insert(QStringLiteral("captionButtons"), m_captionButtons->state());
    }

    const QRect screen(mapToGlobal(QPoint(0, 0)), size());
    return QVariantMap{
        {QStringLiteral("height"), height()},
        {QStringLiteral("expectedHeight"), kUnifiedBarHeight},
        {QStringLiteral("screenRect"),
         QVariantList{screen.x(), screen.y(), screen.width(), screen.height()}},
        // Distance from the top of the window to the top of the bar.  Must be
        // 0: anything else means something is reserving a strip above the
        // unified bar, which is exactly the "wasted top row" this design exists
        // to remove.
        {QStringLiteral("offsetInWindow"),
         window() ? mapTo(window(), QPoint(0, 0)).y() : -1},
        {QStringLiteral("minimalMode"), m_minimalMode},
        {QStringLiteral("brand"),
         QVariantMap{
             {QStringLiteral("wordmark"),
              m_brand ? m_brand->wordmarkText() : QString()},
             {QStringLiteral("logoLoaded"), m_brand && m_brand->hasLogo()},
             {QStringLiteral("visible"), m_brand && m_brand->isVisible()},
         }},
        {QStringLiteral("radios"), m_radioTabs ? m_radioTabs->state() : QVariantMap{}},
        {QStringLiteral("audio"), audio},
        {QStringLiteral("chrome"), chrome},
        {QStringLiteral("txTimer"), txTimerState()},
    };
}

void TitleBar::markDragHandle(QWidget* widget)
{
    if (!widget) return;
    widget->setProperty(kTitleDragHandleProperty, true);
    widget->setCursor(Qt::OpenHandCursor);
    widget->installEventFilter(this);
}

bool TitleBar::isDragHandle(QObject* obj) const
{
    return obj && obj->property(kTitleDragHandleProperty).toBool();
}

bool TitleBar::isSystemMoveAreaAt(const QPoint& globalPos) const
{
    const QPoint localPos = mapFromGlobal(globalPos);
    if (!rect().contains(localPos)) {
        return false;
    }

    QWidget* child = childAt(localPos);
    if (!child) {
        return true;
    }

    if (m_menuBar && (child == m_menuBar || m_menuBar->isAncestorOf(child))) {
        const QPoint menuPos = m_menuBar->mapFromGlobal(globalPos);
        return !m_menuBar->actionAt(menuPos);
    }

    for (QWidget* widget = child; widget && widget != this;
         widget = widget->parentWidget()) {
        if (isDragHandle(widget)) {
            return true;
        }
    }

    return false;
}

bool TitleBar::startWindowMove(QMouseEvent* ev, bool useSystemMove)
{
    if (!ev || ev->button() != Qt::LeftButton)
        return false;

    auto* w = window();
    if (!w)
        return false;

    if (useSystemMove) {
#ifdef Q_OS_WIN
        HWND hwnd = reinterpret_cast<HWND>(w->winId());
        if (hwnd) {
            const QPoint globalPos = ev->globalPosition().toPoint();
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                         MAKELPARAM(globalPos.x(), globalPos.y()));
            ev->accept();
            return true;
        }
#else
        // startSystemMove() reports success on xcb but the WM-driven drag it
        // hands off to is unreliable there (QTBUG-69716) — under Mutter/
        // XWayland (the common case for `QT_QPA_PLATFORM=xcb` on a Wayland
        // desktop) the press is swallowed and the window just never follows
        // the pointer (#4827). Skip straight to the manual-move path below,
        // same rule Qt's own QSizeGrip::usePlatformSizeGrip() applies.
        if (!FramelessMoveHelper::systemMoveResizeUnreliable(w)) {
            if (auto* h = w->windowHandle())
                if (h->startSystemMove()) {
                    m_windowMoveActive = true;
                    m_windowMoveUsesSystem = true;
                    ev->accept();
                    return true;
                }
        }
#endif
    }

    // Manual-move path: when a child-widget eventFilter consumes the press
    // (returns true), Qt never establishes an implicit grab on the child,
    // so subsequent mouse-move events stop reaching us as soon as the
    // cursor leaves the widget that was clicked.  Explicitly grab on
    // TitleBar so all moves/releases route to our handlers.
    m_windowMoveActive = true;
    m_windowMoveUsesSystem = false;
    m_windowMovePressGlobal = ev->globalPosition().toPoint();
    m_windowMoveStartPos = w->pos();
    grabMouse();

    ev->accept();
    return true;
}

bool TitleBar::continueWindowMove(QMouseEvent* ev)
{
    if (!m_windowMoveActive || !ev)
        return false;

    if (!(ev->buttons() & Qt::LeftButton))
        return finishWindowMove(ev);

    if (!m_windowMoveUsesSystem) {
        if (auto* w = window()) {
            const QPoint delta = ev->globalPosition().toPoint() - m_windowMovePressGlobal;
            w->move(m_windowMoveStartPos + delta);
        }
    }

    ev->accept();
    return true;
}

bool TitleBar::finishWindowMove(QMouseEvent* ev)
{
    if (!m_windowMoveActive)
        return false;

    const bool wasManual = !m_windowMoveUsesSystem;
    m_windowMoveActive = false;
    m_windowMoveUsesSystem = false;
    if (wasManual)
        releaseMouse();
    if (ev)
        ev->accept();
    return true;
}

void TitleBar::handleTitleDoubleClick(QMouseEvent* ev)
{
    if (!ev || ev->button() != Qt::LeftButton)
        return;

    if (m_minimalMode) {
        emit minimalModeWindowedExitRequested();
        ev->accept();
        return;
    }

    if (auto* w = window()) {
        if (w->isMaximized()) w->showNormal();
        else                  w->showMaximized();
        ev->accept();
    }
}

void TitleBar::setAppletPanelControlsVisible(bool visible)
{
    if (m_dockLeftLbl)  m_dockLeftLbl->setVisible(visible);
    if (m_dockRightLbl) m_dockRightLbl->setVisible(visible);
    if (m_popOutLbl)    m_popOutLbl->setVisible(visible);
    // The trio is BRACKETED by separators; with the icons gone the two
    // dividers sit adjacent and one dangles (8600 field report).  The
    // trailing one belongs to the cluster and hides with it.
    if (m_dockSep)      m_dockSep->setVisible(visible);
}

bool TitleBar::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == window() && ev->type() == QEvent::WindowStateChange) {
        updateMaximizeIcon();
        return QWidget::eventFilter(obj, ev);
    }

    if (m_windowMoveActive) {
        if (ev->type() == QEvent::MouseMove)
            return continueWindowMove(static_cast<QMouseEvent*>(ev));
        if (ev->type() == QEvent::MouseButtonRelease)
            return finishWindowMove(static_cast<QMouseEvent*>(ev));
    }

    if (obj == m_menuBar) {
        if (ev->type() == QEvent::MouseButtonDblClick) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton && !m_menuBar->actionAt(me->pos())) {
                handleTitleDoubleClick(me);
                return me->isAccepted();
            }
        } else if (ev->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton && !m_menuBar->actionAt(me->pos()))
                return startWindowMove(me);
        }
    }

    // Drag-handle press / double-click: do NOT intercept here.  Letting
    // the event bubble to TitleBar's own mousePressEvent /
    // mouseDoubleClickEvent gives us a working startSystemMove path with
    // proper grab semantics (intercepting from a child eventFilter does
    // not establish an implicit grab, so manual w->move() loses the
    // cursor as soon as it leaves the originally-pressed widget).  The
    // markDragHandle() cursor change still applies for the affordance
    // hint, and the m_windowMoveActive branch above still routes
    // child-widget mouse moves during an in-progress drag.

    if (ev->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() == Qt::LeftButton) {
            if (obj == m_dockLeftLbl) {
                emit dockAppletLeftRequested();
                return true;
            }
            if (obj == m_dockRightLbl) {
                emit dockAppletRightRequested();
                return true;
            }
            if (obj == m_popOutLbl) {
                emit popOutAppletRequested();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void TitleBar::setAppletFloating(bool floating)
{
    m_appletPanelFloating = floating;
    if (m_popOutLbl)
        m_popOutLbl->setPixmap(buildPopOutIcon(m_popOutLbl, floating));
}

void TitleBar::setAppletDockState(bool visible, bool left)
{
    m_appletPanelVisible = visible;
    m_appletPanelDockedLeft = left;
    if (m_dockLeftLbl)
        m_dockLeftLbl->setPixmap(
            buildDockSideIcon(m_dockLeftLbl, true, visible && left));
    if (m_dockRightLbl)
        m_dockRightLbl->setPixmap(
            buildDockSideIcon(m_dockRightLbl, false, visible && !left));
}

void TitleBar::updateMaximizeIcon()
{
    if (!m_captionButtons) {
        return;   // macOS — AppKit owns the traffic lights' own glyphs.
    }
    auto* w = window();
    // In minimal mode the control leaves minimal mode rather than restoring, so
    // it keeps the un-maximized square regardless of the real window state.
    m_captionButtons->setMaximized(!m_minimalMode && w && w->isMaximized());
}

void TitleBar::mousePressEvent(QMouseEvent* ev)
{
    // Bare title-bar gaps arrive here; non-interactive child widgets are
    // tagged in markDragHandle() and routed through eventFilter().
    if (startWindowMove(ev))
        return;
    QWidget::mousePressEvent(ev);
}

void TitleBar::mouseMoveEvent(QMouseEvent* ev)
{
    if (continueWindowMove(ev))
        return;
    QWidget::mouseMoveEvent(ev);
}

void TitleBar::mouseReleaseEvent(QMouseEvent* ev)
{
    if (finishWindowMove(ev))
        return;
    QWidget::mouseReleaseEvent(ev);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* ev)
{
    // Standard Linux/Windows convention: double-click the title bar to
    // toggle maximize.  Same child-passthrough rule as mousePressEvent.
    handleTitleDoubleClick(ev);
    if (ev->isAccepted())
        return;
    QWidget::mouseDoubleClickEvent(ev);
}

void TitleBar::setMenuBar(QMenuBar* mb)
{
    if (!mb) return;
    AetherSDR::ThemeManager::instance().applyStyleSheet(mb, "QMenuBar { background: transparent; color: {{color.text.secondary}}; font-size: 12px; }"
        "QMenuBar::item { padding: 4px 8px; }"
        "QMenuBar::item:selected { background: {{color.background.1}}; color: {{color.text.primary}}; }"
        "QMenu { background: {{color.background.0}}; color: {{color.text.primary}}; border: 1px solid {{color.background.2}}; }"
        "QMenu::item:selected { background: {{color.background.2}}; }"
        "QMenu::separator { height: 1px; background: {{color.background.2}}; margin: 4px 8px; }");
    mb->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    m_menuBar = mb;
    m_menuBar->installEventFilter(this);
    // Between the brand mark and the radio tabs — the brand always leads the
    // bar, so the menu can't be at index 0 any more.
    m_hbox->insertWidget(m_menuBarSlot, mb);
}

void TitleBar::setPcAudioLocked(bool locked)
{
    if (!m_pcBtn)
        return;
    if (locked)
        setPcAudioEnabled(true);      // locked ON, never locked off
    m_pcBtn->setEnabled(!locked);
    if (locked) {
        m_pcBtn->setToolTip(
            tr("PC audio is required: this radio's audio is produced and "
               "captured on this computer, so it cannot be turned off."));
    } else {
        updatePcAudioToolTip();
    }
}

void TitleBar::setPcAudioEnabled(bool on)
{
    QSignalBlocker b(m_pcBtn);
    m_pcBtn->setChecked(on);
    applyPcAudioStyle();
}

void TitleBar::applyPcAudioStyle()
{
    if (!m_pcBtn) {
        return;
    }
    const QString style = m_pcBtn->isChecked()
        ? QStringLiteral(
              "QPushButton { background: {{color.background.success}};"
              " color: {{color.text.primary}}; border: 1px solid {{color.accent.success}};"
              " border-radius: 12px; font-size: 10px; font-weight: bold; }"
              "QPushButton:hover { background: {{color.background.success}}; }")
        : QStringLiteral(
              "QPushButton { background: {{color.background.1}};"
              " color: {{color.text.secondary}}; border: 1px solid {{color.border.strong}};"
              " border-radius: 12px; font-size: 10px; font-weight: bold; }"
              "QPushButton:hover { background: {{color.background.2}}; }");
    ThemeManager::instance().applyStyleSheet(m_pcBtn, style);
}

void TitleBar::setPcAudioDevices(const QString& inputDevice, const QString& outputDevice)
{
    m_pcAudioInputDevice = inputDevice.trimmed();
    m_pcAudioOutputDevice = outputDevice.trimmed();
    updatePcAudioToolTip();
}

void TitleBar::updatePcAudioToolTip()
{
    if (!m_pcBtn)
        return;

    const QString input = m_pcAudioInputDevice.isEmpty()
        ? tr("Unavailable")
        : m_pcAudioInputDevice;
    const QString output = m_pcAudioOutputDevice.isEmpty()
        ? tr("Unavailable")
        : m_pcAudioOutputDevice;

    m_pcBtn->setToolTip(
        tr("Routes radio receive audio through this computer.\n"
           "PC mic transmit uses the selected input device.\n"
           "Input: %1\n"
           "Output: %2")
            .arg(input, output));
}

void TitleBar::setLineoutMuted(bool muted)
{
    QSignalBlocker b(m_speakerBtn);
    m_speakerBtn->setChecked(muted);
    applyAudioIcon(m_speakerBtn, AudioIcon::Speaker, muted);
}

void TitleBar::setHeadphoneMuted(bool muted)
{
    QSignalBlocker b(m_headphoneBtn);
    m_headphoneBtn->setChecked(muted);
    // Explicit, not redundant (#4722): the blocker above suppresses `toggled`,
    // which is what normally swaps the mark, so without this the button's
    // checked state and its mark disagree. Was a setText() of a 🔇/🎧 emoji
    // before the bar's marks became painter-drawn icons; the contract is
    // unchanged, only what carries it.
    applyAudioIcon(m_headphoneBtn, AudioIcon::Headphone, muted);
}

void TitleBar::setMasterVolume(int pct)
{
    QSignalBlocker b(m_masterSlider);
    m_masterSlider->setValue(pct);
    m_masterLabel->setText(QString::number(pct));
}

void TitleBar::setHeadphoneVolume(int pct)
{
    QSignalBlocker b(m_hpSlider);
    m_hpSlider->setValue(pct);
    m_hpLabel->setText(QString::number(pct));
}

void TitleBar::setMultiFlexStatus(int count, const QStringList& names)
{
    if (count > 0) {
        m_mfBtn->setVisible(true);
        QString tip = QString("multiFLEX — %1 other client%2:\n")
            .arg(count).arg(count > 1 ? "s" : "");
        for (const QString& n : names)
            tip += "  " + n + "\n";
        m_mfBtn->setToolTip(tip.trimmed());
    } else {
        m_mfBtn->setVisible(false);
    }
}

void TitleBar::setOtherClientTx(bool transmitting, const QString& station)
{
    if (transmitting && !station.isEmpty()) {
        m_otherTxLabel->setText(QString("TX %1").arg(station));
        m_otherTxLabel->setVisible(true);
    } else {
        m_otherTxLabel->setVisible(false);
    }
}

void TitleBar::setExperimentalRadioFamily(const QString& familyName)
{
    if (!m_experimentalRadioLabel) {
        return;
    }

    const QString trimmed = familyName.trimmed();
    const bool experimental = !trimmed.isEmpty();
    m_experimentalRadioLabel->setToolTip(
        experimental
            ? QStringLiteral("%1 radio support is experimental").arg(trimmed)
            : QString());
    m_experimentalRadioLabel->setAccessibleDescription(
        experimental
            ? QStringLiteral("Connected to %1; some controls, meters, and features may be "
                             "incomplete")
                  .arg(trimmed)
            : QString());
    m_experimentalRadioLabel->setVisible(experimental);
}

QString TitleBar::formatTxElapsed(qint64 ms) const
{
    const qint64 totalSec = ms / 1000;
    const qint64 h = totalSec / 3600;
    const qint64 m = (totalSec % 3600) / 60;
    const qint64 s = totalSec % 60;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

void TitleBar::updateTxTimerText()
{
    const qint64 ms = m_txTimerRunning ? m_txElapsed.elapsed() : m_txFrozenMs;
    m_txTimerLabel->setText(formatTxElapsed(ms));
}

void TitleBar::setOperatorTransmitting(bool active)
{
    if (active) {
        if (m_txTimerRunning)
            return;   // duplicate rising edge — keep the in-progress timer
                      // running rather than resetting the elapsed clock to 0.
                      // A genuine new over is always preceded by an unkey
                      // (falling edge) that clears m_txTimerRunning first.
        // Key-up: cancel any pending hold/fade, restart from 0:00 (each
        // transmission is its own timer), and show at full opacity.
        m_txTimerHoldTimer->stop();
        m_txTimerFade->stop();
        m_txTimerOpacity->setOpacity(1.0);
        m_txTimerRunning = true;
        m_txFrozenMs = 0;
        m_txElapsed.restart();
        updateTxTimerText();
        m_txTimerLabel->setVisible(true);
        m_txTimerTick->start();
        return;
    }

    if (!m_txTimerRunning)
        return;   // spurious/duplicate unkey — nothing to freeze

    // Unkey: freeze the final elapsed time, stop ticking, and hold the reading
    // on-screen for 15s before fading out.
    m_txFrozenMs = m_txElapsed.elapsed();
    m_txTimerRunning = false;
    m_txTimerTick->stop();
    updateTxTimerText();
    m_txTimerOpacity->setOpacity(1.0);
    m_txTimerLabel->setVisible(true);
    m_txTimerHoldTimer->start();
}

QVariantMap TitleBar::txTimerState() const
{
    const qint64 ms = m_txTimerRunning ? m_txElapsed.elapsed() : m_txFrozenMs;
    return QVariantMap{
        {QStringLiteral("visible"), m_txTimerLabel && m_txTimerLabel->isVisible()},
        {QStringLiteral("running"), m_txTimerRunning},
        {QStringLiteral("holding"), m_txTimerHoldTimer && m_txTimerHoldTimer->isActive()},
        {QStringLiteral("fading"),
            m_txTimerFade && m_txTimerFade->state() == QAbstractAnimation::Running},
        {QStringLiteral("elapsedMs"), ms},
        // Derive text from the same live `ms` as elapsedMs so the two never
        // disagree (the label itself is only repainted at 5 Hz). (#4131 review)
        {QStringLiteral("text"), formatTxElapsed(ms)},
        {QStringLiteral("opacity"), m_txTimerOpacity ? m_txTimerOpacity->opacity() : 0.0},
    };
}

void TitleBar::showFeatureRequestDialog()
{
    // Version check guard (#486) — warn if not on latest release
    auto* nam = new QNetworkAccessManager(this);
    // Bound the version check (#4688 §6). Without it a half-open connection to
    // api.github.com leaves the reply pending for the lifetime of the window,
    // holding the manager and the lambda's captures with it.
    nam->setTransferTimeout(kTransferTimeoutMs);
    auto* reply = nam->get(QNetworkRequest(
        QUrl("https://api.github.com/repos/aethersdr/AetherSDR/releases/latest")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam] {
        reply->deleteLater();
        nam->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            auto doc = QJsonDocument::fromJson(reply->readAll());
            QString latest = doc.object().value("tag_name").toString();
            if (latest.startsWith('v')) latest = latest.mid(1);
            auto latestVer = VersionNumber::parse(latest);
            auto currentVer = VersionNumber::parse(QCoreApplication::applicationVersion());
            if (!latestVer.isNull() && currentVer < latestVer) {
                auto answer = FramelessMessageBox::warning(this, "Outdated Version",
                    QString("<p>You are running <b>v%1</b> but <b>v%2</b> is available.</p>"
                            "<p>Your issue may already be fixed in the latest release. "
                            "Please update before filing a bug report.</p>"
                            "<p>Continue anyway?</p>")
                        .arg(QCoreApplication::applicationVersion(), latest),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (answer != QMessageBox::Yes) return;
            }
        }
        // Proceed to show the feature request dialog
        showFeatureRequestDialogImpl();
    });
}

void TitleBar::showFeatureRequestDialogImpl()
{
    static const QString kPrompt =
        "IMPORTANT — before doing anything else, fetch the complete list of open\n"
        "issues by reading pages sequentially until you get fewer than 100 results:\n"
        "  Page 1: https://github.com/aethersdr/AetherSDR/issues?state=open&per_page=100&page=1\n"
        "  Page 2: https://github.com/aethersdr/AetherSDR/issues?state=open&per_page=100&page=2\n"
        "  ... continue until a page returns fewer than 100 issues.\n"
        "Do NOT rely on cached or training data for the issue list.\n\n"
        "Also fetch CLAUDE.md fresh (do not use cached versions):\n"
        "  https://raw.githubusercontent.com/aethersdr/AetherSDR/main/CLAUDE.md\n\n"
        "I want to report an issue or request a feature for AetherSDR, a cross-platform\n"
        "Qt6/C++20 client for FlexRadio transceivers. It uses the FlexLib API over TCP/UDP.\n\n"
        "DUPLICATE CHECK — this is mandatory. Search the fetched issue list for keywords\n"
        "related to my description below. Check titles AND bodies. If you find an existing\n"
        "issue that covers the same thing, STOP and tell me:\n"
        "  > Duplicate found: #<number> — <title>\n"
        "  > I recommend adding a +1 reaction and a comment describing your use case.\n"
        "Do NOT write a new issue if a duplicate exists.\n\n"
        "If no duplicate exists, determine whether my description is a BUG REPORT or a\n"
        "FEATURE REQUEST, then write a GitHub issue using the appropriate format below.\n"
        "Use GitHub-flavored Markdown formatting (headers, code blocks, bullet points).\n\n"
        "FOR FEATURE REQUESTS include:\n"
        "1. A clear, concise title (imperative mood)\n"
        "2. ## What — what the feature does from the user's perspective\n"
        "3. ## Why — what problem it solves\n"
        "4. ## How Other Clients Do It — how SmartSDR, GQRX, SDR++, etc. handle this\n"
        "5. ## Suggested Behavior — specific UX: what the user clicks, sees, what happens.\n"
        "   Reference AetherSDR UI elements (AppletPanel, VfoWidget, RxApplet, etc.)\n"
        "6. ## Protocol Hints — relevant FlexLib commands, or \"Unknown — needs research\"\n"
        "7. ## Acceptance Criteria — 3-5 bullet points defining done vs not-done\n\n"
        "FOR BUG REPORTS include:\n"
        "1. A clear title describing the broken behavior\n"
        "2. ## What happened — describe the incorrect behavior\n"
        "3. ## What I expected — describe the correct behavior\n"
        "4. ## Steps to reproduce — numbered steps to trigger the bug\n"
        "5. ## Environment — OS, radio model, firmware version if relevant\n"
        "6. ## Suggested fix — if you have an idea what's wrong, describe it\n\n"
        "Suggest appropriate labels from: enhancement, bug, audio, GUI, spectrum,\n"
        "protocol, external devices, upstream, SmartLink, windows, macOS\n\n"
        "Here is my idea or bug report:\n\n"
        "[Describe your feature or bug here in plain English]";

    // Reuse existing dialog if still open
    if (m_issueReporterDialog) {
        m_issueReporterDialog->raise();
        m_issueReporterDialog->activateWindow();
        return;
    }

    auto* dlg = new PersistentDialog(QStringLiteral("AI-Assisted Issue Reporter"),
                                     QStringLiteral("IssueReporterDialogGeometry"), this);
    m_issueReporterDialog = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    AetherSDR::ThemeManager::instance().applyStyleSheet(dlg, "QDialog { background: {{color.background.0}}; }");
    dlg->setMinimumWidth(620);

    auto* vbox = new QVBoxLayout(dlg->bodyWidget());
    vbox->setSpacing(8);
    vbox->setContentsMargins(16, 16, 16, 16);
    dlg->setBodyLayoutMargins(QMargins(16, 16, 16, 16),
                              QMargins(16, 14, 16, 16));

    auto* header = new QLabel(
        "<h3 style='color:#c8d8e8;'>AI-Assisted Issue Reporter</h3>"
        "<p style='color:#8090a0;'>Use any AI assistant to write a detailed bug report or feature request.</p>"
        "<ol style='color:#c8d8e8;'>"
        "<li><b>Choose your AI</b> below — prompt is copied to your clipboard</li>"
        "<li><b>Paste the prompt</b> into the AI chat</li>"
        "<li><b>Describe your idea</b> — edit the [bracketed] section</li>"
        "<li><b>Copy the AI's output</b> and click <b>Submit Your Idea</b></li>"
        "</ol>");
    header->setWordWrap(true);
    vbox->addWidget(header);

    // Status label — shows after provider selected
    auto* statusLabel = new QLabel;
    statusLabel->setStyleSheet("QLabel { color: #20c060; font-size: 11px; font-weight: bold; }");
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->hide();
    vbox->addWidget(statusLabel);

    // AI provider buttons
    const QString btnStyle =
        "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
        "border-radius: 3px; color: #c8d8e8; font-size: 12px; font-weight: bold; "
        "padding: 6px 12px; }"
        "QPushButton:hover { background: #203040; }";

    auto* btnRow1 = new QHBoxLayout;
    struct { const char* name; const char* url; } providers[] = {
        {"Claude",     "https://claude.ai/new"},
        {"ChatGPT",    "https://chat.openai.com/"},
        {"Gemini",     "https://gemini.google.com/"},
        {"Grok",       "https://grok.x.ai/"},
        {"Perplexity", "https://www.perplexity.ai/"},
    };
    for (const auto& p : providers) {
        auto* btn = new QPushButton(p.name, dlg);
        btn->setStyleSheet(btnStyle);
        QString url = p.url;
        const QString& prompt = kPrompt;  // local ref for lambda capture
        connect(btn, &QPushButton::clicked, dlg, [url, statusLabel, prompt] {
            QApplication::clipboard()->setText(prompt);
            QDesktopServices::openUrl(QUrl(url));
            statusLabel->setText("Prompt copied to clipboard — paste into the AI, "
                                 "then come back and click Submit Your Idea");
            statusLabel->show();
        });
        btnRow1->addWidget(btn);
    }
    vbox->addLayout(btnRow1);

    vbox->addSpacing(8);

    // Submit / Report / Close
    auto* btnRow2 = new QHBoxLayout;

    auto* submitBtn = new QPushButton("Submit Your Idea", dlg);
    AetherSDR::ThemeManager::instance().applyStyleSheet(submitBtn, "QPushButton { background: {{color.accent}}; color: {{color.background.0}}; font-weight: bold; "
        "border-radius: 4px; padding: 8px 20px; font-size: 13px; }"
        "QPushButton:hover { background: {{color.accent.bright}}; }");
    connect(submitBtn, &QPushButton::clicked, dlg, [dlg] {
        QDesktopServices::openUrl(QUrl(
            "https://github.com/aethersdr/AetherSDR/issues/new?template=feature_request.yml"));
        QTimer::singleShot(500, dlg, &QDialog::close);
    });
    btnRow2->addWidget(submitBtn);

    auto* bugBtn = new QPushButton("Report a Bug", dlg);
    AetherSDR::ThemeManager::instance().applyStyleSheet(bugBtn, "QPushButton { background: #cc4040; color: {{color.text.primary}}; font-weight: bold; "
        "border-radius: 4px; padding: 8px 20px; font-size: 13px; }"
        "QPushButton:hover { background: #dd5050; }");
    connect(bugBtn, &QPushButton::clicked, dlg, [dlg] {
        QDesktopServices::openUrl(QUrl(
            "https://github.com/aethersdr/AetherSDR/issues/new?template=bug_report.yml"));
        QTimer::singleShot(500, dlg, &QDialog::close);
    });
    btnRow2->addWidget(bugBtn);

    auto* closeBtn = new QPushButton("Close", dlg);
    closeBtn->setStyleSheet(btnStyle);
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    btnRow2->addWidget(closeBtn);
    vbox->addLayout(btnRow2);

    // Copy prompt to clipboard on first open
    QApplication::clipboard()->setText(kPrompt);

    dlg->show();
}

void TitleBar::setChildDialogsFramelessMode(bool on)
{
    if (m_issueReporterDialog) {
        m_issueReporterDialog->setFramelessMode(on);
    }
}

// Colour the active tab's dot should show for the current link state, or an
// invalid colour to mean "leave the tab's own status colour alone".  Discovery
// and loss both speak over the radio's status; a healthy link does not need to.
QColor TitleBar::linkOverrideColor() const
{
    if (m_missedBeats >= kHeartbeatAlarmThreshold) {
        return ThemeManager::instance().color(
            this, QStringLiteral("color.accent.danger"));
    }
    if (m_discovering) {
        return ThemeManager::instance().color(
            this, QStringLiteral("color.accent.warning"));
    }
    return QColor();
}

void TitleBar::pushLinkIndicator()
{
    if (!m_radioTabs) {
        return;
    }
    m_radioTabs->setLinkIndicator(linkOverrideColor(),
                                  m_missedBeats >= kHeartbeatAlarmThreshold);
}

void TitleBar::setDiscovering(bool active)
{
    if (m_discovering == active) {
        return;
    }
    m_discovering = active;
    pushLinkIndicator();
}

QColor TitleBar::currentBeatColor() const
{
    return m_throttleFlashColor.isEmpty()
        ? ThemeManager::instance().color(this, QStringLiteral("color.accent.success"))
        : QColor(m_throttleFlashColor);
}

void TitleBar::onHeartbeat()
{
    m_discovering = false;
    m_missedBeats = 0;
    pushLinkIndicator();
    // One swell of the active tab's glow per discovery packet.  The pulse is
    // data now, not decoration: a link that stops answering simply stops
    // breathing, which is the same signal the old 100 ms flash carried.
    if (m_radioTabs) {
        m_radioTabs->pulseLink(currentBeatColor());
    }
}

void TitleBar::setThrottleFlashColor(const QString& color)
{
    if (m_throttleFlashColor == color) return;
    m_throttleFlashColor = color;
    // The alarm owns the indicator while it is up — never fight it.  The new
    // tint takes effect on the next beat, which is also when it becomes true.
    if (m_missedBeats >= kHeartbeatAlarmThreshold) return;
    pushLinkIndicator();
}

void TitleBar::onHeartbeatLost()
{
    m_missedBeats++;
    if (m_missedBeats == kHeartbeatAlarmThreshold) {
        // Crossing the threshold is the edge that raises the alarm.  Whether it
        // blinks or holds solid red is RadioTabBar's call, driven by the same
        // blink preference — and it holds solid when blinking is off, because
        // an operator who silenced the animation still has to see a lost link.
        pushLinkIndicator();
    }
}

void TitleBar::setBlinkEnabled(bool enabled)
{
    if (m_blinkEnabled == enabled) return;
    m_blinkEnabled = enabled;
    AppSettings::instance().setValue("HeartbeatBlinkEnabled", enabled ? "True" : "False");
    AppSettings::instance().save();
    emit blinkEnabledChanged(enabled);
    // One switch governs every animated status light in the bar.
    if (m_radioTabs) {
        m_radioTabs->setPulseEnabled(enabled);
    }
    pushLinkIndicator();
}

void TitleBar::setMinimalMode(bool on)
{
    m_minimalMode = on;

    // Hide non-essential controls so status badges fit in the narrow strip.
    if (m_menuBar) m_menuBar->setVisible(!on);
    if (m_brand) m_brand->setVisible(!on);
    // The radio strip stays, compacted to the active tab: its dot is the
    // radio-link indicator now, and minimal mode is exactly when an operator
    // has nothing else on screen that would show them a dropped link.
    if (m_radioTabs) m_radioTabs->setCompactMode(on);
    m_pcBtn->setVisible(!on);
    m_speakerBtn->setVisible(!on);
    m_headphoneBtn->setVisible(!on);
    m_masterSlider->setVisible(!on);
    m_hpSlider->setVisible(!on);
    m_masterLabel->setVisible(!on);
    m_hpLabel->setVisible(!on);
    // Dock-side selectors lose meaning in minimal mode — no panel
    // layout to dock or pop out of.
    if (m_dockLeftLbl)  m_dockLeftLbl->setVisible(!on);
    if (m_dockRightLbl) m_dockRightLbl->setVisible(!on);
    if (m_popOutLbl)    m_popOutLbl->setVisible(!on);
    if (m_dockSep)      m_dockSep->setVisible(!on);
    // Don't touch m_otherTxLabel or m_mfBtn — their visibility is
    // managed by setOtherClientTx() and setMultiFlexStatus() respectively
    updateMaximizeIcon();
}

} // namespace AetherSDR
