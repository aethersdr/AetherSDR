#include "TitleBar.h"
#include "GuardedSlider.h"
#include "core/AppSettings.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QVBoxLayout>
#include <QDialog>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QSlider>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QDesktopServices>
#include <QClipboard>
#include <QApplication>
#include <QTimer>
#include <QWindow>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include "core/VersionNumber.h"

namespace AetherSDR {

namespace {
constexpr const char* kTitleDragHandleProperty = "aetherTitleDragHandle";

// Build a 16×18 dock-side indicator: hollow rectangle (the main window)
// with a thin shaded strip flush against one inner wall, representing
// the applet panel docked on that side.  Visual language matches the
// pop-out icon's thin-strip-as-panel motif.
QPixmap buildDockSideIcon(bool fillLeft, bool active)
{
    QPixmap pm(16, 18);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QColor stroke(active ? QColor(255, 255, 255, 230)
                                : QColor(138, 168, 192, 200));
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
QPixmap buildPopOutIcon(bool active)
{
    QPixmap pm(16, 18);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QColor stroke(active ? QColor(255, 255, 255, 230)
                                : QColor(138, 168, 192, 200));
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
}

TitleBar::TitleBar(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(32);
    setStyleSheet("TitleBar { background: #0a0a14; border-bottom: 1px solid #203040; }");

    m_hbox = new QHBoxLayout(this);
    m_hbox->setContentsMargins(4, 2, 8, 2);
    m_hbox->setSpacing(6);

    auto makeDragGutter = [this]() {
        auto* gutter = new QWidget(this);
        gutter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        gutter->setMinimumWidth(0);
        markDragHandle(gutter);
        return gutter;
    };

    // Keep the identity/status cluster anchored on the left, immediately after
    // the menu bar once it is inserted via setMenuBar().

    // ── Heartbeat indicator ─────────────────────────────────────────────────
    m_heartbeat = new QLabel;
    m_heartbeat->setFixedSize(10, 10);
    m_heartbeat->setStyleSheet(
        "QLabel { background: #404858; border-radius: 5px; }");
    m_heartbeat->setToolTip("Radio discovery heartbeat");
    m_heartbeat->setAccessibleName("Radio heartbeat");
    m_heartbeat->setAccessibleDescription("Flashes green when radio discovery packets are received");
    markDragHandle(m_heartbeat);

    // 100ms timer to return green flash back to grey
    m_heartbeatOffTimer = new QTimer(this);
    m_heartbeatOffTimer->setSingleShot(true);
    m_heartbeatOffTimer->setInterval(100);
    connect(m_heartbeatOffTimer, &QTimer::timeout, this, [this]() {
        m_heartbeat->setStyleSheet(
            "QLabel { background: #404858; border-radius: 5px; }");
    });

    // 500ms alarm blink timer (red/grey alternating)
    m_heartbeatAlarmTimer = new QTimer(this);
    m_heartbeatAlarmTimer->setInterval(500);
    connect(m_heartbeatAlarmTimer, &QTimer::timeout, this, [this]() {
        m_alarmRed = !m_alarmRed;
        m_heartbeat->setStyleSheet(m_alarmRed
            ? "QLabel { background: #cc2020; border-radius: 5px; }"
            : "QLabel { background: #404858; border-radius: 5px; }");
    });

    // Load persisted blink preference (default: enabled)
    m_blinkEnabled = AppSettings::instance()
        .value("HeartbeatBlinkEnabled", "True").toString() == "True";

    // Right-click on the indicator to toggle blink on/off without opening a menu
    m_heartbeat->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_heartbeat, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        // Heap-allocate with WA_DeleteOnClose so the menu outlives this lambda.
        // Use popup() not exec() — exec() creates a nested event loop which can
        // allow network/connection events to be processed out of order while the
        // menu is open. popup() is non-blocking and safe during connection setup.
        QMenu* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        QAction* blinkAction = menu->addAction("Blink status indicator");
        blinkAction->setCheckable(true);
        blinkAction->setChecked(m_blinkEnabled);
        connect(blinkAction, &QAction::triggered, this, [this](bool checked) {
            setBlinkEnabled(checked);
        });
        menu->popup(m_heartbeat->mapToGlobal(pos));
    });

    // On Linux/Windows the menu bar occupies the left side, so add a stretch
    // to center the app name. On macOS the menu is at the OS level, so the
    // app name stays flush left.
#ifndef Q_OS_MAC
    // Linux/Windows: center the identity cluster (menu bar is on the left)
    m_hbox->addWidget(makeDragGutter(), 1);
    m_hbox->addWidget(m_heartbeat);
    m_hbox->addSpacing(4);
#endif

    m_appNameLabel = new QLabel(
        QString("AetherSDR v%1").arg(QCoreApplication::applicationVersion()));
    m_appNameLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 14px; font-weight: bold; }");
    m_appNameLabel->setAlignment(Qt::AlignCenter);
    markDragHandle(m_appNameLabel);
    m_hbox->addWidget(m_appNameLabel);

    m_mfBtn = new QPushButton("multiFLEX");
    m_mfBtn->setFlat(true);
    m_mfBtn->setStyleSheet(
        "QPushButton { color: #20c060; font-size: 11px; font-weight: bold; "
        "border: 1px solid #20c060; border-radius: 4px; "
        "background: transparent; padding: 0px 3px; }"
        "QPushButton:hover { background: rgba(32, 192, 96, 30); }");
    m_mfBtn->setVisible(false);
    m_mfBtn->setCursor(Qt::PointingHandCursor);
    m_mfBtn->setAccessibleName("multiFLEX status");
    connect(m_mfBtn, &QPushButton::clicked, this, &TitleBar::multiFlexClicked);
    m_hbox->addWidget(m_mfBtn);

#ifdef Q_OS_MAC
    // macOS: heartbeat after multiFLEX (left-aligned, no menu bar in title)
    m_hbox->addSpacing(4);
    m_hbox->addWidget(m_heartbeat);
#endif

    m_hbox->addWidget(makeDragGutter(), 1);

    // ── Right: Other client TX indicator + PC Audio + Master Vol + HP Vol ──
    m_otherTxLabel = new QLabel();
    m_otherTxLabel->setStyleSheet(
        "QLabel { background: white; color: #cc0000; font-size: 12px; "
        "font-weight: bold; border-radius: 3px; padding: 2px 8px; }");
    m_otherTxLabel->setVisible(false);
    markDragHandle(m_otherTxLabel);
    m_hbox->addWidget(m_otherTxLabel);

    // ── PC Audio + Master Vol + HP Vol ──────────────────────────────────────
    auto& s = AppSettings::instance();

    // PC Audio toggle
    m_pcBtn = new QPushButton("PC Audio");
    m_pcBtn->setCheckable(true);
    m_pcBtn->setFixedHeight(22);
    m_pcBtn->setFixedWidth(70);

    bool pcOn = s.value("PcAudioEnabled", "True").toString() == "True";
    m_pcBtn->setChecked(pcOn);
    m_pcBtn->setAccessibleName("PC Audio");
    m_pcBtn->setAccessibleDescription("Toggle PC audio receive playback");
    updatePcAudioToolTip();

    auto updatePcStyle = [this]() {
        m_pcBtn->setStyleSheet(m_pcBtn->isChecked()
            ? "QPushButton { background: #1a6030; color: #40ff80; border: 1px solid #20a040; "
              "border-radius: 3px; font-size: 10px; font-weight: bold; }"
              "QPushButton:hover { background: #207040; }"
            : "QPushButton { background: #1a2a3a; color: #607080; border: 1px solid #304050; "
              "border-radius: 3px; font-size: 10px; font-weight: bold; }"
              "QPushButton:hover { background: #243848; }");
    };
    updatePcStyle();

    connect(m_pcBtn, &QPushButton::toggled, this, [this, updatePcStyle](bool on) {
        updatePcStyle();
        auto& ss = AppSettings::instance();
        ss.setValue("PcAudioEnabled", on ? "True" : "False");
        ss.save();
        emit pcAudioToggled(on);
    });
    m_hbox->addWidget(m_pcBtn);

    m_hbox->addSpacing(8);

    // Master volume (click icon to mute/unmute)
    m_speakerBtn = new QPushButton("\xF0\x9F\x94\x8A");  // 🔊
    m_speakerBtn->setFixedSize(20, 20);
    m_speakerBtn->setCheckable(true);
    m_speakerBtn->setStyleSheet(
        "QPushButton { background: transparent; border: none; font-size: 14px; padding: 0; }"
        "QPushButton:checked { opacity: 0.4; }");
    m_speakerBtn->setToolTip("Click to mute/unmute line out");
    m_speakerBtn->setAccessibleName("Line out mute");
    m_speakerBtn->setAccessibleDescription("Mute or unmute line out speaker audio");
    connect(m_speakerBtn, &QPushButton::toggled, this, [this](bool muted) {
        m_speakerBtn->setText(muted ? "\xF0\x9F\x94\x87" : "\xF0\x9F\x94\x8A");  // 🔇 / 🔊
        emit lineoutMuteChanged(muted);
    });
    m_hbox->addWidget(m_speakerBtn);

    m_masterSlider = new GuardedSlider(Qt::Horizontal);
    m_masterSlider->setRange(0, 100);
    int savedVol = s.value("MasterVolume", "100").toInt();
    m_masterSlider->setValue(savedVol);
    m_masterSlider->setFixedWidth(80);
    m_masterSlider->setFixedHeight(16);
    m_masterSlider->setAccessibleName("Master volume");
    m_masterSlider->setAccessibleDescription("Line out volume level, 0 to 100 percent");
    m_masterSlider->setStyleSheet(
        "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #00b4d8; width: 10px; margin: -3px 0; border-radius: 5px; }"
        "QSlider::sub-page:horizontal { background: #00b4d8; border-radius: 2px; }");
    m_hbox->addWidget(m_masterSlider);

    m_masterLabel = new QLabel(QString::number(savedVol));
    m_masterLabel->setFixedWidth(22);
    m_masterLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 10px; }");
    m_masterLabel->setAlignment(Qt::AlignCenter);
    markDragHandle(m_masterLabel);
    m_hbox->addWidget(m_masterLabel);

    connect(m_masterSlider, &QSlider::valueChanged, this, [this](int v) {
        m_masterLabel->setText(QString::number(v));
        emit masterVolumeChanged(v);
    });

    m_hbox->addSpacing(8);

    // Headphone volume (click icon to mute/unmute)
    m_headphoneBtn = new QPushButton("\xF0\x9F\x8E\xA7");  // 🎧
    m_headphoneBtn->setFixedSize(20, 20);
    m_headphoneBtn->setCheckable(true);
    m_headphoneBtn->setStyleSheet(
        "QPushButton { background: transparent; border: none; font-size: 14px; padding: 0; }"
        "QPushButton:checked { opacity: 0.4; }");
    m_headphoneBtn->setToolTip("Click to mute/unmute headphones");
    m_headphoneBtn->setAccessibleName("Headphone mute");
    m_headphoneBtn->setAccessibleDescription("Mute or unmute headphone audio");
    connect(m_headphoneBtn, &QPushButton::toggled, this, [this](bool muted) {
        m_headphoneBtn->setText(muted ? "\xF0\x9F\x94\x87" : "\xF0\x9F\x8E\xA7");  // 🔇 / 🎧
        emit headphoneMuteChanged(muted);
    });
    m_hbox->addWidget(m_headphoneBtn);

    m_hpSlider = new GuardedSlider(Qt::Horizontal);
    m_hpSlider->setRange(0, 100);
    m_hpSlider->setValue(50);
    m_hpSlider->setFixedWidth(80);
    m_hpSlider->setFixedHeight(16);
    m_hpSlider->setAccessibleName("Headphone volume");
    m_hpSlider->setAccessibleDescription("Headphone volume level, 0 to 100 percent");
    m_hpSlider->setStyleSheet(
        "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #00b4d8; width: 10px; margin: -3px 0; border-radius: 5px; }"
        "QSlider::sub-page:horizontal { background: #00b4d8; border-radius: 2px; }");
    m_hbox->addWidget(m_hpSlider);

    m_hpLabel = new QLabel("50");
    m_hpLabel->setFixedWidth(22);
    m_hpLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 10px; }");
    m_hpLabel->setAlignment(Qt::AlignCenter);
    markDragHandle(m_hpLabel);
    m_hbox->addWidget(m_hpLabel);

    connect(m_hpSlider, &QSlider::valueChanged, this, [this](int v) {
        m_hpLabel->setText(QString::number(v));
        emit headphoneVolumeChanged(v);
    });

    // ── Window-control trio (min / max / close) ───────────────────────────
    // Discord-style: thin 1 px vertical separator before the trio, larger
    // flat labels with a subtle hover background.  Click is wired via
    // eventFilter().  Close uses a red hover for the destructive action cue.
    m_hbox->addSpacing(8);

    auto* sep = new QFrame;
    sep->setFixedSize(1, 20);
    sep->setStyleSheet("QFrame { background: #304050; border: none; }");
    markDragHandle(sep);
    m_hbox->addWidget(sep);

    m_hbox->addSpacing(4);

    const QString winLblStyle = QStringLiteral(
        "QLabel { color: #8aa8c0; font-size: 18px; padding: 0 10px; "
        "border-radius: 4px; }"
        "QLabel:hover { color: #ffffff; background: #203040; }");
    const QString winCloseLblStyle = QStringLiteral(
        "QLabel { color: #8aa8c0; font-size: 18px; padding: 0 10px; "
        "border-radius: 4px; }"
        "QLabel:hover { color: #ffffff; background: #cc2030; }");
    const QString dockLblStyle = QStringLiteral(
        "QLabel { padding: 0 6px; border-radius: 4px; }"
        "QLabel:hover { background: #203040; }");

    // Dock-side selectors (applet panel left vs right of the panadapter).
    // Click is wired via eventFilter() like the min/max/close trio.
    m_dockLeftLbl = new QLabel;
    m_dockLeftLbl->setFixedHeight(24);
    m_dockLeftLbl->setAlignment(Qt::AlignCenter);
    m_dockLeftLbl->setCursor(Qt::PointingHandCursor);
    m_dockLeftLbl->setToolTip("Dock applet panel to the left of the panadapter");
    m_dockLeftLbl->setStyleSheet(dockLblStyle);
    m_dockLeftLbl->setPixmap(buildDockSideIcon(/*fillLeft=*/true, /*active=*/false));
    m_dockLeftLbl->installEventFilter(this);
    m_hbox->addWidget(m_dockLeftLbl);

    m_dockRightLbl = new QLabel;
    m_dockRightLbl->setFixedHeight(24);
    m_dockRightLbl->setAlignment(Qt::AlignCenter);
    m_dockRightLbl->setCursor(Qt::PointingHandCursor);
    m_dockRightLbl->setToolTip("Dock applet panel to the right of the panadapter");
    m_dockRightLbl->setStyleSheet(dockLblStyle);
    m_dockRightLbl->setPixmap(buildDockSideIcon(/*fillLeft=*/false, /*active=*/true));
    m_dockRightLbl->installEventFilter(this);
    m_hbox->addWidget(m_dockRightLbl);

    m_popOutLbl = new QLabel;
    m_popOutLbl->setFixedHeight(24);
    m_popOutLbl->setAlignment(Qt::AlignCenter);
    m_popOutLbl->setCursor(Qt::PointingHandCursor);
    m_popOutLbl->setToolTip("Pop the applet panel out into its own window");
    m_popOutLbl->setStyleSheet(dockLblStyle);
    m_popOutLbl->setPixmap(buildPopOutIcon(/*active=*/false));
    m_popOutLbl->installEventFilter(this);
    m_hbox->addWidget(m_popOutLbl);

    m_hbox->addSpacing(4);

    m_dockSep = new QFrame;
    m_dockSep->setFixedSize(1, 20);
    m_dockSep->setStyleSheet("QFrame { background: #304050; border: none; }");
    markDragHandle(m_dockSep);
    m_hbox->addWidget(m_dockSep);

    m_hbox->addSpacing(4);

    m_minimizeLbl = new QLabel(QString::fromUtf8("\xe2\x80\x94"));  // — em dash
    m_minimizeLbl->setFixedHeight(24);
    m_minimizeLbl->setAlignment(Qt::AlignCenter);
    m_minimizeLbl->setCursor(Qt::PointingHandCursor);
    m_minimizeLbl->setToolTip("Minimize");
    m_minimizeLbl->setAccessibleName("Minimize window");
    m_minimizeLbl->setStyleSheet(winLblStyle);
    m_minimizeLbl->installEventFilter(this);
    m_hbox->addWidget(m_minimizeLbl);

    m_maximizeLbl = new QLabel(QString::fromUtf8("\xe2\x96\xa1"));  // □ U+25A1
    m_maximizeLbl->setFixedHeight(24);
    m_maximizeLbl->setAlignment(Qt::AlignCenter);
    m_maximizeLbl->setCursor(Qt::PointingHandCursor);
    m_maximizeLbl->setToolTip("Maximize");
    m_maximizeLbl->setAccessibleName("Maximize window");
    m_maximizeLbl->setStyleSheet(winLblStyle);
    m_maximizeLbl->installEventFilter(this);
    m_hbox->addWidget(m_maximizeLbl);

    m_closeLbl = new QLabel(QString::fromUtf8("\xe2\x9c\x95"));  // ✕ U+2715
    m_closeLbl->setFixedHeight(24);
    m_closeLbl->setAlignment(Qt::AlignCenter);
    m_closeLbl->setCursor(Qt::PointingHandCursor);
    m_closeLbl->setToolTip("Close");
    m_closeLbl->setAccessibleName("Close window");
    m_closeLbl->setStyleSheet(winCloseLblStyle);
    m_closeLbl->installEventFilter(this);
    m_hbox->addWidget(m_closeLbl);
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

bool TitleBar::startWindowMove(QMouseEvent* ev, bool useSystemMove)
{
    if (!ev || ev->button() != Qt::LeftButton)
        return false;

    auto* w = window();
    if (!w)
        return false;

    m_windowMoveActive = true;
    m_windowMoveUsesSystem = false;
    m_windowMovePressGlobal = ev->globalPosition().toPoint();
    m_windowMoveStartPos = w->pos();

    if (useSystemMove) {
#ifndef Q_OS_MAC
        if (auto* h = w->windowHandle())
            m_windowMoveUsesSystem = h->startSystemMove();
#endif
    }

    // Manual-move path: when a child-widget eventFilter consumes the press
    // (returns true), Qt never establishes an implicit grab on the child,
    // so subsequent mouse-move events stop reaching us as soon as the
    // cursor leaves the widget that was clicked.  Explicitly grab on
    // TitleBar so all moves/releases route to our handlers.
    if (!m_windowMoveUsesSystem)
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
                return startWindowMove(me, false);
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
            if (obj == m_minimizeLbl) {
                if (auto* w = window()) w->showMinimized();
                return true;
            }
            if (obj == m_maximizeLbl) {
                if (m_minimalMode) {
                    emit minimalModeWindowedExitRequested();
                    return true;
                }
                if (auto* w = window()) {
                    if (w->isMaximized()) w->showNormal();
                    else                  w->showMaximized();
                }
                return true;
            }
            if (obj == m_closeLbl) {
                if (auto* w = window()) w->close();
                return true;
            }
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
    if (m_popOutLbl)
        m_popOutLbl->setPixmap(buildPopOutIcon(floating));
}

void TitleBar::setAppletDockState(bool visible, bool left)
{
    if (m_dockLeftLbl)
        m_dockLeftLbl->setPixmap(buildDockSideIcon(true,  visible &&  left));
    if (m_dockRightLbl)
        m_dockRightLbl->setPixmap(buildDockSideIcon(false, visible && !left));
}

void TitleBar::updateMaximizeIcon()
{
    if (!m_maximizeLbl) return;
    auto* w = window();
    const bool maxed = w && w->isMaximized();
    if (m_minimalMode) {
        m_maximizeLbl->setText(QString::fromUtf8("\xe2\x96\xa1"));
        m_maximizeLbl->setToolTip("Exit Minimal Mode");
        return;
    }
    // ❐ U+2750 (overlapped squares) when maximized → "restore down"
    // □ U+25A1 (single square) when normal → "maximize"
    m_maximizeLbl->setText(maxed
        ? QString::fromUtf8("\xe2\x9d\x90")
        : QString::fromUtf8("\xe2\x96\xa1"));
    m_maximizeLbl->setToolTip(maxed ? "Restore" : "Maximize");
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
    mb->setStyleSheet(
        "QMenuBar { background: transparent; color: #8aa8c0; font-size: 12px; }"
        "QMenuBar::item { padding: 4px 8px; }"
        "QMenuBar::item:selected { background: #203040; color: #ffffff; }"
        "QMenu { background: #0f0f1a; color: #c8d8e8; border: 1px solid #304050; }"
        "QMenu::item:selected { background: #0070c0; }");
    mb->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    m_menuBar = mb;
    m_menuBar->installEventFilter(this);
    // Insert at position 0 (before the first stretch)
    m_hbox->insertWidget(0, mb);
}

void TitleBar::setPcAudioEnabled(bool on)
{
    QSignalBlocker b(m_pcBtn);
    m_pcBtn->setChecked(on);
    m_pcBtn->setStyleSheet(on
        ? "QPushButton { background: #1a6030; color: #40ff80; border: 1px solid #20a040; "
          "border-radius: 3px; font-size: 10px; font-weight: bold; }"
          "QPushButton:hover { background: #207040; }"
        : "QPushButton { background: #1a2a3a; color: #607080; border: 1px solid #304050; "
          "border-radius: 3px; font-size: 10px; font-weight: bold; }"
          "QPushButton:hover { background: #243848; }");
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
    m_speakerBtn->setText(muted ? "\xF0\x9F\x94\x87" : "\xF0\x9F\x94\x8A");  // 🔇 / 🔊
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

void TitleBar::showFeatureRequestDialog()
{
    // Version check guard (#486) — warn if not on latest release
    auto* nam = new QNetworkAccessManager(this);
    auto* reply = nam->get(QNetworkRequest(
        QUrl("https://api.github.com/repos/ten9876/AetherSDR/releases/latest")));
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
                auto answer = QMessageBox::warning(this, "Outdated Version",
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
        "  Page 1: https://github.com/ten9876/AetherSDR/issues?state=open&per_page=100&page=1\n"
        "  Page 2: https://github.com/ten9876/AetherSDR/issues?state=open&per_page=100&page=2\n"
        "  ... continue until a page returns fewer than 100 issues.\n"
        "Do NOT rely on cached or training data for the issue list.\n\n"
        "Also fetch CLAUDE.md fresh (do not use cached versions):\n"
        "  https://raw.githubusercontent.com/ten9876/AetherSDR/main/CLAUDE.md\n\n"
        "I want to report an issue or request a feature for AetherSDR, a Linux-native\n"
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
    static QPointer<QDialog> sDlg;
    if (sDlg) {
        sDlg->raise();
        sDlg->activateWindow();
        return;
    }

    auto* dlg = new QDialog(this);
    sDlg = dlg;
    dlg->setWindowTitle("AI-Assisted Issue Reporter");
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setStyleSheet("QDialog { background: #0f0f1a; }");
    dlg->setMinimumWidth(620);

    auto* vbox = new QVBoxLayout(dlg);
    vbox->setSpacing(8);
    vbox->setContentsMargins(16, 16, 16, 16);

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
    submitBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
        "border-radius: 4px; padding: 8px 20px; font-size: 13px; }"
        "QPushButton:hover { background: #00c8f0; }");
    connect(submitBtn, &QPushButton::clicked, dlg, [dlg] {
        QDesktopServices::openUrl(QUrl(
            "https://github.com/ten9876/AetherSDR/issues/new?template=feature_request.yml"));
        QTimer::singleShot(500, dlg, &QDialog::close);
    });
    btnRow2->addWidget(submitBtn);

    auto* bugBtn = new QPushButton("Report a Bug", dlg);
    bugBtn->setStyleSheet(
        "QPushButton { background: #cc4040; color: #ffffff; font-weight: bold; "
        "border-radius: 4px; padding: 8px 20px; font-size: 13px; }"
        "QPushButton:hover { background: #dd5050; }");
    connect(bugBtn, &QPushButton::clicked, dlg, [dlg] {
        QDesktopServices::openUrl(QUrl(
            "https://github.com/ten9876/AetherSDR/issues/new?template=bug_report.yml"));
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

void TitleBar::setDiscovering(bool active)
{
    m_discovering = active;
    if (active) {
        // Solid amber — discovery in progress, no connection yet
        m_heartbeatOffTimer->stop();
        m_heartbeatAlarmTimer->stop();
        m_heartbeat->setStyleSheet(
            "QLabel { background: #e0a020; border-radius: 5px; }");
        m_heartbeat->setToolTip("Searching for radio…");
    } else {
        m_heartbeat->setToolTip("Radio discovery heartbeat");
        // Return to idle gray — onHeartbeat() will take over once pings arrive
        m_heartbeat->setStyleSheet(
            "QLabel { background: #404858; border-radius: 5px; }");
    }
}

void TitleBar::onHeartbeat()
{
    m_discovering = false;
    m_missedBeats = 0;
    m_heartbeatAlarmTimer->stop();
    m_alarmRed = false;
    m_heartbeat->setToolTip("Radio discovery heartbeat");
    m_heartbeat->setStyleSheet(
        "QLabel { background: #20c060; border-radius: 5px; }");
    if (m_blinkEnabled) {
        m_heartbeatOffTimer->start();  // flash green → gray after 100ms
    }
    // When blink is off: stays static green — no timer, no animation
}

void TitleBar::onHeartbeatLost()
{
    m_missedBeats++;
    if (m_missedBeats >= 3 && !m_heartbeatAlarmTimer->isActive()) {
        m_heartbeatOffTimer->stop();
        if (m_blinkEnabled) {
            m_heartbeatAlarmTimer->start();  // blinking red ↔ gray every 500ms
        } else {
            // Static red — alarm timer is NEVER started.
            // Indicator stays red until the next successful ping restores connection.
            // This is intentional and safety-critical: contest operators must see
            // connection loss clearly, even with blink disabled.
            m_alarmRed = true;
            m_heartbeat->setStyleSheet(
                "QLabel { background: #cc2020; border-radius: 5px; }");
        }
    }
}

void TitleBar::setBlinkEnabled(bool enabled)
{
    if (m_blinkEnabled == enabled) return;
    m_blinkEnabled = enabled;
    AppSettings::instance().setValue("HeartbeatBlinkEnabled", enabled ? "True" : "False");
    AppSettings::instance().save();
    emit blinkEnabledChanged(enabled);

    if (enabled) {
        // Resume alarm blink immediately if currently in alarm state (m_missedBeats >= 3).
        // Without this, re-enabling blink while connection is lost leaves the indicator
        // static red until the next onHeartbeatLost() call increments the counter again.
        if (m_missedBeats >= 3 && !m_heartbeatAlarmTimer->isActive()) {
            m_heartbeatAlarmTimer->start();
        }
        return;
    }

    // Immediately reconcile mid-session: stop any active animation and freeze state
    if (m_heartbeatAlarmTimer->isActive()) {
        // Was blinking red — freeze to solid red (connection lost)
        m_heartbeatAlarmTimer->stop();
        m_alarmRed = true;
        m_heartbeat->setStyleSheet(
            "QLabel { background: #cc2020; border-radius: 5px; }");
    } else if (m_heartbeatOffTimer->isActive()) {
        // Was mid green-flash — freeze to solid green (connected)
        m_heartbeatOffTimer->stop();
        m_heartbeat->setStyleSheet(
            "QLabel { background: #20c060; border-radius: 5px; }");
    }
}

void TitleBar::setMinimalMode(bool on)
{
    m_minimalMode = on;

    // Hide non-essential controls so status badges fit in the narrow strip.
    if (m_menuBar) m_menuBar->setVisible(!on);
    if (m_appNameLabel) m_appNameLabel->setVisible(!on);
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
