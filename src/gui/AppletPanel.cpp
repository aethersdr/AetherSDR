#include "AppletPanel.h"
#include "containers/ContainerManager.h"
#include "containers/ContainerTitleBar.h"
#include "containers/ContainerWidget.h"
#include "GuardedSlider.h"
#include "ComboStyle.h"
#include "RxApplet.h"
#include "SMeterWidget.h"
#include "TunerApplet.h"
#include "AmpApplet.h"
#include "TxApplet.h"
#include "PhoneCwApplet.h"
#include "PhoneApplet.h"
#include "EqApplet.h"
#include "WaveApplet.h"
#include "ClientEqApplet.h"
#include "ClientCompApplet.h"
#include "ClientGateApplet.h"
#include "ClientDeEssApplet.h"
#include "ClientTubeApplet.h"
#include "ClientPuduApplet.h"
#include "ClientReverbApplet.h"
#include "ClientRxDspApplet.h"
#include "ClientChainApplet.h"
#include "CatControlApplet.h"
#include "DaxApplet.h"
#include "TciApplet.h"
#include "DaxIqApplet.h"
#include "AntennaGeniusApplet.h"
#include "ShackSwitchApplet.h"
#include "MeterApplet.h"
#include "ProfileSwitcherApplet.h"
#include "HealthApplet.h"
#include "KiwiSdrApplet.h"
#ifdef HAVE_RADE
#include "RadeApplet.h"
#endif
#ifdef HAVE_MQTT
#include "MqttApplet.h"
#endif
#include "models/SliceModel.h"
#include <QAction>
#include <QActionGroup>
#include <QWidgetAction>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QLabel>
#include <QMenu>
#include "core/AppSettings.h"
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QScrollBar>
#include <QPainter>
#include <QPixmap>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include "core/ThemeManager.h"
#include "FavoritesPickerDialog.h"

namespace AetherSDR {

const QStringList AppletPanel::kDefaultOrder = {
    "RX", "TUN", "AMP", "TX", "PHNE", "P/CW", "EQ", "WAVE", "TXDSP", "CAT", "DAX", "TCI", "IQ", "MTR", "PROF", "KSDR", "HLTH", "AG", "SS"
};

// ── Drop-aware scroll area ──────────────────────────────────────────────────

class AppletDropArea : public QScrollArea {
public:
    AppletDropArea(AppletPanel* panel) : QScrollArea(panel), m_panel(panel) {
        setAcceptDrops(true);
    }

protected:
    void dragEnterEvent(QDragEnterEvent* ev) override {
        if (ev->mimeData()->hasFormat("application/x-aethersdr-applet"))
            ev->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent* ev) override {
        if (!ev->mimeData()->hasFormat("application/x-aethersdr-applet")) return;
        ev->acceptProposedAction();
        m_panel->m_dropIndicator->setVisible(true);

        // Position the indicator at the computed drop index
        int localY = ev->position().toPoint().y() + verticalScrollBar()->value();
        int idx = m_panel->dropIndexFromY(localY);

        // Find the Y position for the indicator
        int indicatorY = 0;
        bool indicatorPlaced = false;
        auto isDockedVisible = [](const AppletPanel::AppletEntry& entry) {
            auto* w = entry.widget;
            if (!w || !w->isVisible()) return false;
            if (auto* cw = qobject_cast<ContainerWidget*>(w); cw && cw->isFloating())
                return false;
            return true;
        };
        for (int i = idx; i < m_panel->m_appletOrder.size(); ++i) {
            const auto& entry = m_panel->m_appletOrder[i];
            if (!isDockedVisible(entry)) continue;
            if (auto* title = entry.titleBar) {
                indicatorY = title->mapTo(widget(), QPoint(0, 0)).y();
                indicatorPlaced = true;
                break;
            }
        }
        if (!indicatorPlaced) {
            for (int i = m_panel->m_appletOrder.size() - 1; i >= 0; --i) {
                const auto& entry = m_panel->m_appletOrder[i];
                if (!isDockedVisible(entry)) continue;
                if (auto* w = entry.widget) {
                    indicatorY = w->mapTo(widget(), QPoint(0, w->height())).y();
                    indicatorPlaced = true;
                    break;
                }
            }
        }
        m_panel->m_dropIndicator->setParent(widget());
        m_panel->m_dropIndicator->setGeometry(4, indicatorY - 1, widget()->width() - 8, 2);
        m_panel->m_dropIndicator->raise();
    }

    void dragLeaveEvent(QDragLeaveEvent*) override {
        m_panel->m_dropIndicator->setVisible(false);
    }

    void dropEvent(QDropEvent* ev) override {
        m_panel->m_dropIndicator->setVisible(false);
        if (!ev->mimeData()->hasFormat("application/x-aethersdr-applet")) return;

        QString draggedId = QString::fromUtf8(ev->mimeData()->data("application/x-aethersdr-applet"));
        int localY = ev->position().toPoint().y() + verticalScrollBar()->value();
        int dropIdx = m_panel->dropIndexFromY(localY);

        // Find the dragged applet's current index.  Composite tiles
        // (e.g. TXDSP) set ContainerWidget::setDragId() at construction
        // so the drag MIME payload matches AppletEntry.id directly — no
        // fallback lookup needed (#3057, supersedes #1836 workaround).
        int srcIdx = -1;
        for (int i = 0; i < m_panel->m_appletOrder.size(); ++i) {
            if (m_panel->m_appletOrder[i].id == draggedId) { srcIdx = i; break; }
        }
        if (srcIdx < 0) return;

        // Adjust drop index if moving down (after removing source)
        if (dropIdx > srcIdx) dropIdx--;
        if (dropIdx == srcIdx) return;

        // Move the entry
        auto entry = m_panel->m_appletOrder[srcIdx];
        m_panel->m_appletOrder.remove(srcIdx);
        m_panel->m_appletOrder.insert(dropIdx, entry);
        m_panel->rebuildStackOrder();
        m_panel->saveOrder();

        ev->acceptProposedAction();
    }

private:
    AppletPanel* m_panel;
};

// ── AppletPanel ──────────────────────────────────────────────────────────────

AppletPanel::AppletPanel(QWidget* parent) : QWidget(parent)
{
    // Parent container for every applet hosted in this rail.  Each
    // individual applet declares its own child scope ("applet.tx",
    // "applet.rx", …) inside its own constructor so widgets inside
    // an applet inherit applet.<name> → applet → root.
    theme::setContainer(this, QStringLiteral("applet"));

    setFixedWidth(260);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Top button bar: single favorites row + push-down drawer ────────────
    //
    // The bar shows kFavoriteCount (5) user-chosen "favorite" buttons in a
    // single row, with a 6th slot acting as the drawer toggle (▾/▴).
    // Toggling the drawer reveals/hides a grid containing every other
    // bar button.  Right-click any bar button to open the picker dialog.
    //
    // Construction: every bar button (LCK, VU, RX, …) is created with the
    // drawer's widget+layout as its rowParent/rowLayout.  Once all are
    // registered, applyBarLayout() lifts the favorites out of the drawer
    // into m_favRow.  See registerBarButton() / applyBarLayout().
    //
    // All colour values resolved through ThemeManager so the bar
    // re-themes live alongside the rest of the UI.
    const QString kBarContainerStyle =
        QStringLiteral("QWidget#barContainer { background: {{color.background.0}}; }");
    const QString kBarBtnStyle =
        QStringLiteral(
            "QPushButton { background: {{color.background.1}}; "
            "border: 1px solid {{color.background.2}}; border-radius: 3px; "
            "padding: 2px 3px; font-size: 11px; font-weight: bold; "
            "color: {{color.text.primary}}; }"
            "QPushButton:hover { background: {{color.background.2}}; }"
            // Active/checked state mirrors the kBlueActive style used by
            // the RxApplet filter-preset buttons (1.8K/2.1K/…): dim cyan
            // fill, light text, slightly brighter outline.
            "QPushButton:checked { background: {{color.accent.dim}}; "
            "color: {{color.text.primary}}; "
            "border: 1px solid {{color.accent.bright}}; }"
            "QPushButton:disabled { color: {{color.text.disabled}}; "
            "border-color: {{color.background.1}}; }");

    m_favRow = new QWidget;
    m_favRow->setObjectName("barContainer");
    ThemeManager::instance().applyStyleSheet(m_favRow, kBarContainerStyle + kBarBtnStyle);
    m_favLayout = new QGridLayout(m_favRow);
    m_favLayout->setContentsMargins(2, 3, 2, 3);
    m_favLayout->setSpacing(2);
    root->addWidget(m_favRow);

    m_drawer = new QWidget;
    m_drawer->setObjectName("barContainer");
    ThemeManager::instance().applyStyleSheet(m_drawer, kBarContainerStyle + kBarBtnStyle +
        QStringLiteral("QWidget#barContainer { border-bottom: 1px solid {{color.border.subtle}}; }"));
    m_drawerLayout = new QGridLayout(m_drawer);
    m_drawerLayout->setContentsMargins(2, 0, 2, 3);
    m_drawerLayout->setSpacing(2);
    m_drawer->hide();
    root->addWidget(m_drawer);

    // Drawer toggle button — always occupies the last slot of the
    // favorites row.  Right-click also opens the favorites picker
    // so users can reach it without clicking through to find a favorite.
    // Uses full-size triangle glyphs (▼/▲) at a larger font size so the
    // affordance reads at panel-bar scale; the small caret glyphs (▾/▴)
    // were nearly invisible at the bar's 11px default.
    m_drawerToggleBtn = new QPushButton(QString::fromUtf8("\xe2\x96\xbc"), m_favRow); // ▼
    m_drawerToggleBtn->setCheckable(true);
    m_drawerToggleBtn->setToolTip("Show / hide additional buttons\nRight-click: customize favorites");
    m_drawerToggleBtn->setAccessibleName("Toggle button drawer");
    m_drawerToggleBtn->setAccessibleDescription(
        "Show or hide the panel of non-favorite buttons");
    m_drawerToggleBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_drawerToggleBtn->setFixedHeight(20);
    m_drawerToggleBtn->setStyleSheet("QPushButton { font-size: 14px; padding: 0px; }");
    m_drawerToggleBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_drawerToggleBtn, &QPushButton::toggled,
            this, &AppletPanel::setDrawerOpen);
    connect(m_drawerToggleBtn, &QWidget::customContextMenuRequested,
            this, [this](const QPoint&){ openFavoritesPicker(); });

    // Phase 4a (#1713) — container system groundwork.  Created early
    // so the S-Meter (and any future root-level containers) can wrap
    // themselves here.  The root "sidebar" container is a hidden peer
    // reserved for later phases; existing applets still live in the
    // legacy m_stack alongside it.
    m_containerMgr = new ContainerManager(this);
    m_rootSidebar = m_containerMgr->createContainer("sidebar", "Sidebar");
    m_rootSidebar->titleBar()->setCloseButtonVisible(false);
    m_rootSidebar->hide();

    // ── S-Meter section (wrapped in a ContainerWidget like every
    //    other applet — #1713 Phase 4d) ─────────────────────────────
    auto* sMeterContent = new QWidget;
    auto* contentLayout = new QVBoxLayout(sMeterContent);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    m_sMeter = new SMeterWidget(sMeterContent);
    m_sMeter->setAccessibleName("S-Meter");
    m_sMeter->setAccessibleDescription("Signal strength meter, shows S-units or TX power");
    contentLayout->addWidget(m_sMeter);

    // ── S-meter config → right-click context menu on the gauge ────────────
    // The applet body is just the gauge now; TX/RX meter select, peak-hold
    // toggle, decay speed, and reset all live in the right-click menu so the
    // VU applet stays compact. The menu reads/writes the same AppSettings keys
    // (SMeter_TxSelect/RxSelect, PeakHoldEnabled, PeakDecayRate) it always has,
    // so state survives and stays in sync.

    // Apply decay preset: also sets the hold time (Fast=200ms, Medium=500ms, Slow=1000ms)
    auto applyDecayPreset = [this](const QString& rate) {
        m_sMeter->setPeakDecayRate(rate);
        if (rate == "Fast")        m_sMeter->setPeakHoldTimeMs(200);
        else if (rate == "Slow")   m_sMeter->setPeakHoldTimeMs(1000);
        else                       m_sMeter->setPeakHoldTimeMs(500);
    };

    static const QStringList kTxMeterItems{"Power", "SWR", "Level", "Compression"};
    static const QStringList kRxMeterItems{"S-Meter", "S-Meter Peak"};
    static const QStringList kDecayItems{"Fast", "Medium", "Slow"};

    // Restore saved state onto the gauge at startup (#809, #840).
    {
        auto& s = AppSettings::instance();
        const int txIdx = qBound(0, s.value("SMeter_TxSelect", 0).toInt(),
                                 static_cast<int>(kTxMeterItems.size()) - 1);
        const int rxIdx = qBound(0, s.value("SMeter_RxSelect", 0).toInt(),
                                 static_cast<int>(kRxMeterItems.size()) - 1);
        m_sMeter->setTxMode(kTxMeterItems[txIdx]);
        m_sMeter->setRxMode(kRxMeterItems[rxIdx]);
        m_sMeter->setPeakHoldEnabled(s.value("PeakHoldEnabled", "False") == "True");
        applyDecayPreset(s.value("PeakDecayRate", "Medium").toString());
    }

    m_sMeter->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sMeter, &QWidget::customContextMenuRequested, this,
            [this, applyDecayPreset](const QPoint& pos) {
        auto& s = AppSettings::instance();
        QMenu menu(m_sMeter);

        // Section header rows. QMenu::addSection() doesn't render its text under
        // the app's menu styling (only the separator shows), so use a disabled
        // QWidgetAction + styled QLabel so the section titles are always visible.
        bool firstHeader = true;
        auto addHeader = [&menu, &firstHeader](const QString& text) {
            if (!firstHeader)
                menu.addSeparator();
            firstHeader = false;
            auto* lbl = new QLabel(text);
            lbl->setStyleSheet(
                "color:#8090a0; font-size:10px; font-weight:bold; "
                "padding:4px 8px 2px 8px;");
            auto* wa = new QWidgetAction(&menu);
            wa->setDefaultWidget(lbl);
            wa->setEnabled(false);
            menu.addAction(wa);
        };

        // TX Select (exclusive)
        addHeader("TX Select");
        auto* txGroup = new QActionGroup(&menu);
        const int txCur = s.value("SMeter_TxSelect", 0).toInt();
        for (int i = 0; i < kTxMeterItems.size(); ++i) {
            QAction* a = menu.addAction(kTxMeterItems[i]);
            a->setCheckable(true);
            a->setChecked(i == txCur);
            txGroup->addAction(a);
            connect(a, &QAction::triggered, this, [this, i] {
                m_sMeter->setTxMode(kTxMeterItems[i]);
                AppSettings::instance().setValue("SMeter_TxSelect", i);
                AppSettings::instance().save();
            });
        }

        // RX Select (exclusive)
        addHeader("RX Select");
        auto* rxGroup = new QActionGroup(&menu);
        const int rxCur = s.value("SMeter_RxSelect", 0).toInt();
        for (int i = 0; i < kRxMeterItems.size(); ++i) {
            QAction* a = menu.addAction(kRxMeterItems[i]);
            a->setCheckable(true);
            a->setChecked(i == rxCur);
            rxGroup->addAction(a);
            connect(a, &QAction::triggered, this, [this, i] {
                m_sMeter->setRxMode(kRxMeterItems[i]);
                AppSettings::instance().setValue("SMeter_RxSelect", i);
                AppSettings::instance().save();
            });
        }

        // Peak Hold (toggle)
        addHeader("Peak Hold");
        QAction* peakAct = menu.addAction("Enabled");
        peakAct->setCheckable(true);
        peakAct->setChecked(s.value("PeakHoldEnabled", "False") == "True");
        connect(peakAct, &QAction::toggled, this, [this](bool on) {
            m_sMeter->setPeakHoldEnabled(on);
            AppSettings::instance().setValue("PeakHoldEnabled", on ? "True" : "False");
            AppSettings::instance().save();
        });

        // Decay speed (exclusive)
        addHeader("Decay speed");
        auto* decayGroup = new QActionGroup(&menu);
        const QString decayCur = s.value("PeakDecayRate", "Medium").toString();
        for (const QString& d : kDecayItems) {
            QAction* a = menu.addAction(d);
            a->setCheckable(true);
            a->setChecked(d == decayCur);
            decayGroup->addAction(a);
            connect(a, &QAction::triggered, this, [this, d, applyDecayPreset] {
                applyDecayPreset(d);
                AppSettings::instance().setValue("PeakDecayRate", d);
                AppSettings::instance().save();
            });
        }

        // Reset Peak Hold
        menu.addSeparator();
        QAction* rstAct = menu.addAction("Reset Peak Hold");
        connect(rstAct, &QAction::triggered, m_sMeter, &SMeterWidget::resetPeak);

        menu.exec(m_sMeter->mapToGlobal(pos));
    });

    // One-shot migration: legacy Applet_ANLG visibility key → Applet_VU.
    // Run before reading Applet_VU so the first launch after upgrade
    // picks up the user's prior on/off state.
    {
        auto& s = AppSettings::instance();
        if (!s.contains("Applet_VU") && s.contains("Applet_ANLG")) {
            s.setValue("Applet_VU", s.value("Applet_ANLG", "True").toString());
            s.remove("Applet_ANLG");
        }
    }

    // Wrap the S-Meter content in a container and park it at the top
    // of the sidebar (outside the reorderable applet stack).  The
    // container's own titlebar provides drag/float/close; the VU
    // tray button toggles its visibility.
    m_sMeterContainer = m_containerMgr->createContainer("VU", "S-Meter");
    m_sMeterContainer->setContent(sMeterContent);
    const bool sMeterOn = AppSettings::instance()
        .value("Applet_VU", "True").toString() == "True";
    m_sMeterContainer->setContainerVisible(sMeterOn);
    root->addWidget(m_sMeterContainer);

    // ── Scrollable applet stack (drop-aware) ────────────────────────────────
    m_scrollArea = new AppletDropArea(this);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setWidgetResizable(true);
    // Handle dims to #2a3a4a at rest, brightens to #4a6880 on hover/drag.
    // 500 ms delay before dimming back so quick drags don't flicker.
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_scrollArea, "QScrollBar:vertical { background: {{color.background.0}}; width: 12px; }"
        "QScrollBar::handle:vertical { background: {{color.background.1}}; border-radius: 4px; min-height: 20px; }"
        "QScrollBar::handle:vertical[active=\"true\"] { background: #4a6880; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    if (auto* sb = m_scrollArea->verticalScrollBar()) {
        sb->setProperty("active", false);
        sb->installEventFilter(this);
    }
    m_scrollDimTimer = new QTimer(this);
    m_scrollDimTimer->setSingleShot(true);
    m_scrollDimTimer->setInterval(500);
    connect(m_scrollDimTimer, &QTimer::timeout, this,
            [this]() { setScrollHandleActive(false); });

    // Override sizeHint/minimumSizeHint so QScrollArea::setWidgetResizable
    // always sizes this container to the viewport width rather than "sticking"
    // at the AppletPanel's fixed width (260px).  Without this, the VBox layout's
    // sizeHint stays at 260px after the initial layout pass, and the 12px
    // scrollbar track (always visible) pushes the viewport to 248px — but Qt
    // keeps the container at max(248, sizeHint=260)=260px, silently clipping
    // the right 12px of every applet header (including the × close button).
    struct FlexContainer : QWidget {
        using QWidget::QWidget;
        QSize sizeHint() const override
            { return QSize(0, QWidget::sizeHint().height()); }
        QSize minimumSizeHint() const override
            { return QSize(0, QWidget::minimumSizeHint().height()); }
    };
    auto* container = new FlexContainer;
    m_stack = new QVBoxLayout(container);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->setSpacing(0);
    // Stretch factor 1 (not the default 0) so all surplus vertical space is
    // routed to this trailing spacer.  With a factor-0 spacer, Qt distributes
    // surplus equally among every item whose expandingDirections() includes
    // Vertical — which is any tile whose body contains a vertically-expanding
    // child — parking a blank band below those headers (#3461/#4098).  A
    // non-zero factor here pins every container to its sizeHint and collects
    // the slack at the bottom, regardless of a tile's internal size policy.
    m_stack->addStretch(1);
    m_scrollArea->setWidget(container);
    root->addWidget(m_scrollArea, 1);

    // Drop indicator line (cyan, hidden by default)
    m_dropIndicator = new QWidget(container);
    m_dropIndicator->setFixedHeight(2);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_dropIndicator, "background: {{color.accent}};");
    m_dropIndicator->hide();

    auto& settings = AppSettings::instance();

    // Migrate Applet_DIGI → Applet_CAT on first run after the DIGI split (#1627).
    // DAX/TCI/IQ default off regardless — only the CAT tile inherits the old
    // DIGI visibility because the CAT button replaces DIGI's slot in the tray.
    if (settings.contains("Applet_DIGI") && !settings.contains("Applet_CAT")) {
        settings.setValue("Applet_CAT", settings.value("Applet_DIGI", "False").toString());
        settings.remove("Applet_DIGI");
        settings.save();
    }

    // ── Build all applets with title bars ────────────────────────────────────

    // Helper: create an applet entry with draggable title bar
    // Event filter to initiate drag from existing applet title bars (top 16px).
    // Installed on each wrapper widget.
    class DragFilter : public QObject {
    public:
        DragFilter(const QString& id, QWidget* parent) : QObject(parent), m_id(id) {}
    protected:
        bool eventFilter(QObject* obj, QEvent* ev) override {
            auto* w = qobject_cast<QWidget*>(obj);
            if (!w) return false;
            if (ev->type() == QEvent::MouseButtonPress) {
                auto* me = static_cast<QMouseEvent*>(ev);
                if (me->button() == Qt::LeftButton && me->pos().y() < 18)
                    m_dragStart = me->pos();
            } else if (ev->type() == QEvent::MouseMove) {
                auto* me = static_cast<QMouseEvent*>(ev);
                if ((me->buttons() & Qt::LeftButton) && !m_dragStart.isNull()
                    && (me->pos() - m_dragStart).manhattanLength() > 10) {
                    auto* drag = new QDrag(w);
                    auto* mimeData = new QMimeData;
                    mimeData->setData("application/x-aethersdr-applet", m_id.toUtf8());
                    drag->setMimeData(mimeData);
                    QPixmap pm(w->width(), 16);
                    pm.fill(Qt::transparent);
                    w->render(&pm, QPoint(), QRegion(0, 0, w->width(), 16));
                    drag->setPixmap(pm);
                    drag->setHotSpot(me->pos());
                    drag->exec(Qt::MoveAction);
                    m_dragStart = {};
                    return true;
                }
            } else if (ev->type() == QEvent::MouseButtonRelease) {
                m_dragStart = {};
            }
            return false;
        }
    private:
        QString m_id;
        QPoint m_dragStart;
    };

    // Post-Phase-4c: each applet lives inside a ContainerWidget with
    // its own ContainerTitleBar (drag handle + float + close buttons).
    // `contentOrContainer` may be either the raw applet widget (we
    // wrap it in a new leaf container) or an already-built
    // ContainerWidget (used by composite tiles like TXDSP whose
    // structure was built outside this helper) — qobject_cast picks
    // the right path.
    auto makeEntry = [&](const QString& id, const QString& label,
                         QWidget* contentOrContainer, bool defaultOn,
                         QWidget* rowParent, QLayout* rowLayout,
                         const QString& buttonText = QString()) -> AppletEntry {
        ContainerWidget* c =
            qobject_cast<ContainerWidget*>(contentOrContainer);
        if (!c) {
            c = m_containerMgr->createContainer(id, label);
            c->setContent(contentOrContainer);
        }

        QPushButton* btn = nullptr;
        // rowLayout != nullptr is the legacy "this applet has a bar button"
        // signal — we now register the button into m_barButtons and let
        // applyBarLayout() decide whether it lands in the favorites
        // row or the drawer.  The rowParent/rowLayout args themselves
        // are ignored (kept in the signature only to minimise churn at
        // the call sites).
        (void)rowParent;
        if (rowLayout) {
            const QString visible = buttonText.isEmpty() ? id : buttonText;
            btn = new QPushButton(visible, m_drawer);
            btn->setCheckable(true);
            registerBarButton(id, visible, label, btn);
        }

        const QString key = QStringLiteral("Applet_%1").arg(id);
        bool on = settings.value(key, defaultOn ? "True" : "False").toString() == "True";
        if (btn) btn->setChecked(on);
        c->setContainerVisible(on);

        // One-shot legacy-float migration: if the old
        // FloatingApplet_<ID>_IsFloating key says this applet was
        // floating before the container refactor, route it through
        // the container manager so the float state carries over.
        // The key is read once; the new ContainerManager persistence
        // takes over from that point.
        // Sanitize '/' in IDs like "P/CW" — System A's floatKey() helper
        // replaced '/' with '_', so match that exact key format here.
        QString safeId = id;
        safeId.replace('/', '_');
        const QString legacyFloatKey =
            QStringLiteral("FloatingApplet_%1_IsFloating").arg(safeId);
        if (settings.value(legacyFloatKey, "False").toString() == "True" && on) {
            QTimer::singleShot(0, this, [this, id, legacyFloatKey]() {
                if (!m_containerMgr) return;
                m_containerMgr->floatContainer(id);
                AppSettings::instance().remove(legacyFloatKey);
                AppSettings::instance().save();
            });
        }

        if (btn) {
            connect(btn, &QPushButton::toggled, this,
                    [this, id, c, key](bool checked) {
                // Floating containers: raising = show the window,
                // lowering = hide it.  The manager owns the window
                // so we just toggle the container's visibility.
                if (c->isFloating()) {
                    if (auto* w = c->window())
                        w->setVisible(checked);
                    return;
                }
                c->setContainerVisible(checked);
                AppSettings::instance().setValue(key, checked ? "True" : "False");
            });
        }

        // Propagate visibility changes (e.g. driven by the close
        // button on the ContainerTitleBar) back to the tray toggle
        // and settings so everything stays in sync.
        connect(c, &ContainerWidget::visibilityChanged, this,
                [this, btn, key](bool visible) {
            if (btn) {
                QSignalBlocker b(btn);
                btn->setChecked(visible);
            }
            AppSettings::instance().setValue(key, visible ? "True" : "False");
        });

        return {id, c, c->titleBar(), btn};
    };

    // Controls lock toggle — disables wheel/mouse on sidebar sliders (#745)
    {
        m_lockBtn = new QPushButton("LOCK", m_drawer);
        m_lockBtn->setCheckable(true);
        m_lockBtn->setToolTip("Lock sidebar controls — prevent accidental\n"
                              "value changes while scrolling");
        m_lockBtn->setAccessibleName("Lock controls");
        m_lockBtn->setAccessibleDescription("Lock sidebar controls to prevent accidental value changes");
        // LCK is session-only: start every app launch unlocked and do not
        // restore or persist the prior session's state.
        AppSettings::instance().remove("ControlsLocked");
        m_lockBtn->setChecked(false);
        ControlsLock::setLocked(false);
        // ID stays "LCK" so saved layouts from earlier builds still match.
        registerBarButton("LCK", "LOCK", "Lock sidebar controls", m_lockBtn);
        connect(m_lockBtn, &QPushButton::toggled, this, [this](bool on) {
            setControlsLocked(on);
        });
    }

    // VU button — toggles the S-Meter container (not in the
    // reorderable stack; sits permanently at the top of the sidebar).
    {
        m_vuBtn = new QPushButton("VU", m_drawer);
        m_vuBtn->setCheckable(true);
        m_vuBtn->setChecked(sMeterOn);
        registerBarButton("VU", "VU", "S-Meter", m_vuBtn);

        connect(m_vuBtn, &QPushButton::toggled, this, [this](bool on) {
            if (!m_sMeterContainer) return;
            // When floating, toggle the floating window visibility
            // instead of the (empty) panel slot.
            if (m_sMeterContainer->isFloating()) {
                if (auto* w = m_sMeterContainer->window())
                    w->setVisible(on);
                return;
            }
            m_sMeterContainer->setContainerVisible(on);
            AppSettings::instance().setValue(
                "Applet_VU", on ? "True" : "False");
        });

        // Keep the button in sync with close-button / external
        // visibility changes.
        connect(m_sMeterContainer, &ContainerWidget::visibilityChanged,
                this, [this](bool visible) {
            if (m_vuBtn) {
                QSignalBlocker b(m_vuBtn);
                m_vuBtn->setChecked(visible);
            }
            AppSettings::instance().setValue(
                "Applet_VU", visible ? "True" : "False");
        });

        // Legacy float-state migration: if the user had the S-Meter
        // popped out under the old FloatingAppletWindow path, carry
        // that over to the container system on first launch.
        if (sMeterOn && settings.value(
                "FloatingApplet_VU_IsFloating", "False").toString() == "True") {
            QTimer::singleShot(0, this, [this]() {
                if (!m_containerMgr) return;
                m_containerMgr->floatContainer("VU");
                AppSettings::instance().remove("FloatingApplet_VU_IsFloating");
                AppSettings::instance().save();
            });
        }
    }

    // Create all applets — row 1: core, row 2: accessories/conditional
    m_rxApplet = new RxApplet;
    m_appletOrder.append(makeEntry("RX", "RX Controls", m_rxApplet, true, m_drawer, m_drawerLayout));

    // Tuner / Amp entries use makeEntry like everything else;
    // MainWindow toggles tray-button visibility via setTunerVisible /
    // setAmpVisible once the hardware reports its presence.  Until
    // then the tray button stays hidden and the container itself
    // starts hidden (defaultOn = false).
    m_tunerApplet = new TunerApplet;
    {
        auto entry = makeEntry("TUN", "Tuner", m_tunerApplet, false,
                               m_drawer, m_drawerLayout);
        m_tuneBtn = entry.btn;
        markHardwareConditional("TUN");
        m_appletOrder.append(entry);
    }

    m_ampApplet = new AmpApplet;
    {
        auto entry = makeEntry("AMP", "Amplifier", m_ampApplet, false,
                               m_drawer, m_drawerLayout);
        m_ampBtn = entry.btn;
        markHardwareConditional("AMP");
        m_appletOrder.append(entry);
    }

    m_txApplet = new TxApplet;
    m_appletOrder.append(makeEntry("TX", "TX Controls", m_txApplet, true, m_drawer, m_drawerLayout));

    m_phoneApplet = new PhoneApplet;
    m_appletOrder.append(makeEntry("PHNE", "Phone", m_phoneApplet, true, m_drawer, m_drawerLayout, "PHN"));

    m_phoneCwApplet = new PhoneCwApplet;
    m_appletOrder.append(makeEntry("P/CW", "Phone/CW", m_phoneCwApplet, true, m_drawer, m_drawerLayout));

    m_eqApplet = new EqApplet;
    m_appletOrder.append(makeEntry("EQ", "Equalizer", m_eqApplet, true, m_drawer, m_drawerLayout));

    m_waveApplet = new WaveApplet;
    m_appletOrder.append(makeEntry("WAVE", "Waveform", m_waveApplet, true, m_drawer, m_drawerLayout, "WAV"));

    // CEQ and CMP intentionally have no toggle button in the tray —
    // their visibility follows DSP bypass state, driven externally
    // from the CHAIN widget and the respective floating editors.
    // TX DSP applets — instead of three independent AppletEntries,
    // we wrap them inside a single nested container (#1713 Phase 5).
    // Each applet becomes the content of its own sub-ContainerWidget
    // with a ContainerTitleBar offering per-section float / close;
    // the three sub-containers live inside a parent "tx_dsp"
    // container whose own titlebar is hidden (the outer AppletEntry
    // wrapper provides the group's drag-handle + tray-toggle).
    // Phase 7.1: CEQ split — one Tx-bound copy + one Rx-bound copy.
    // No internal Rx/Tx tab anymore; each tile owns one path for its
    // entire lifetime.
    m_clientEqTxApplet  = new ClientEqApplet(ClientEqApplet::Path::Tx);
    m_clientEqRxApplet  = new ClientEqApplet(ClientEqApplet::Path::Rx);
    m_clientCompApplet   = new ClientCompApplet(ClientCompApplet::Side::Tx);
    m_clientCompRxApplet = new ClientCompApplet(ClientCompApplet::Side::Rx);
    m_clientGateApplet   = new ClientGateApplet(ClientGateApplet::Side::Tx);
    m_clientGateRxApplet = new ClientGateApplet(ClientGateApplet::Side::Rx);
    m_clientDeEssApplet = new ClientDeEssApplet;
    m_clientTubeApplet   = new ClientTubeApplet(ClientTubeApplet::Side::Tx);
    m_clientTubeRxApplet = new ClientTubeApplet(ClientTubeApplet::Side::Rx);
    m_clientPuduApplet   = new ClientPuduApplet(ClientPuduApplet::Side::Tx);
    m_clientPuduRxApplet = new ClientPuduApplet(ClientPuduApplet::Side::Rx);
    m_clientReverbApplet = new ClientReverbApplet;
    m_clientRxDspApplet  = new ClientRxDspApplet;
    m_clientChainApplet = new ClientChainApplet;

    auto* txDsp = m_containerMgr->createContainer(
        "tx_dsp", QString::fromUtf8("Aetherial Audio\xe2\x84\xa2"));
    // Cap the column width so popped-out floating windows don't grow
    // wider than the chain visualisation needs.  Docked columns are
    // already narrower so this is a no-op there.
    if (txDsp) txDsp->setMaximumWidth(280);

    auto makeChildContainer = [this, txDsp](const QString& id,
                                            const QString& title,
                                            QWidget* applet,
                                            int index) {
        auto* child = m_containerMgr->createContainer(
            id, title, /*contentType=*/{}, /*parentId=*/"tx_dsp", index);
        if (child) child->setContent(applet);
        return child;
    };
    // CHAIN lives directly inside the TX DSP container body — no
    // sub-titlebar, no independent pop-out.  Floating the TX DSP
    // parent carries CHAIN along with it.  (CEQ and CMP remain as
    // pop-outable sub-containers since users commonly want them
    // floating while working on the chain.)
    if (txDsp) txDsp->insertChildWidget(-1, m_clientChainApplet);
    makeChildContainer("gate",    "Aetherial TX Gate",       m_clientGateApplet,   -1);
    makeChildContainer("gate-rx", "Aetherial AGC-G",          m_clientGateRxApplet, -1);
    makeChildContainer("ceq",     "Aetherial TX EQ",          m_clientEqTxApplet,   -1);
    makeChildContainer("ceq-rx",  "Aetherial RX EQ",          m_clientEqRxApplet,   -1);
    makeChildContainer("dess",    "Aetherial De-Esser",       m_clientDeEssApplet,  -1);
    makeChildContainer("cmp",     "Aetherial Compressor",     m_clientCompApplet,   -1);
    makeChildContainer("cmp-rx",  "Aetherial AGC-C",          m_clientCompRxApplet, -1);
    makeChildContainer("tube",    "Aetherial Mic-PreAmp",     m_clientTubeApplet,   -1);
    makeChildContainer("tube-rx", "Aetherial Dynamic Tube",   m_clientTubeRxApplet, -1);
    makeChildContainer("pudu",    QStringLiteral("Aetherial TX Voice Processor"), m_clientPuduApplet,   -1);
    makeChildContainer("pudu-rx", QString::fromUtf8("Aetherial RX Poodoo\xe2\x84\xa2"), m_clientPuduRxApplet, -1);
    makeChildContainer("reverb",  "Aetherial FreeVerb",       m_clientReverbApplet, -1);
    makeChildContainer("dsp-rx",  "Aetherial Noise Reduction", m_clientRxDspApplet, -1);

    // One-shot settings migration (#1713 Phase 4b): carry over the
    // legacy Applet_CHAIN visibility to the new Applet_TXDSP key so
    // existing users who had the chain tile showing don't have to
    // re-enable it.  Run once — only fills TXDSP when it's absent.
    if (!settings.contains("Applet_TXDSP")
        && settings.contains("Applet_CHAIN")) {
        settings.setValue(
            "Applet_TXDSP",
            settings.value("Applet_CHAIN", "False").toString());
    }

    // Register the composite tx_dsp container as one tile in the
    // applet tray — users toggle it, drag it, and pop it out as a
    // unit.  Individual section pop-outs happen via each sub-
    // container's own titlebar inside.  Button label is "VUDU" —
    // marketing name for the whole TX-DSP composite.  Settings ID
    // stays TXDSP for persistence.
    {
        auto entry = makeEntry("TXDSP", "VUDU", txDsp, false,
                               m_drawer, m_drawerLayout, "VUDU");
        // Make the composite's drag MIME match its owning AppletEntry.id
        // so the drop handler's fast lookup hits directly.  Container
        // persistence keys still use the internal id ("tx_dsp") — drag
        // identity and container identity are deliberately separate
        // concepts here. (#3057, supersedes #1836 dropEvent fallback)
        if (txDsp) txDsp->setDragId("TXDSP");
        m_appletOrder.append(entry);
    }

    m_catControlApplet = new CatControlApplet;
    {
        auto catEntry = makeEntry("CAT", "CAT Control", m_catControlApplet, false, m_drawer, m_drawerLayout);
        m_appletOrder.append(catEntry);
        // Switch the applet between its simple (docked) and full-table (floating) views.
        if (auto* c = qobject_cast<ContainerWidget*>(catEntry.widget)) {
            connect(c, &ContainerWidget::dockModeChanged, m_catControlApplet,
                    [this](ContainerWidget::DockMode mode) {
                        m_catControlApplet->setFloating(mode == ContainerWidget::DockMode::Floating);
                    });
        }
    }

    m_daxApplet = new DaxApplet;
    m_appletOrder.append(makeEntry("DAX", "DAX Audio", m_daxApplet, false, m_drawer, m_drawerLayout));

    m_tciApplet = new TciApplet;
    m_appletOrder.append(makeEntry("TCI", "TCI Server", m_tciApplet, false, m_drawer, m_drawerLayout));

    m_daxIqApplet = new DaxIqApplet;
    m_appletOrder.append(makeEntry("IQ", "DAX IQ", m_daxIqApplet, false, m_drawer, m_drawerLayout));

    m_meterApplet = new MeterApplet;
    m_appletOrder.append(makeEntry("MTR", "Meters", m_meterApplet, false, m_drawer, m_drawerLayout));

    m_profApplet = new ProfileSwitcherApplet;
    m_appletOrder.append(makeEntry("PROF", "Profile Switcher", m_profApplet, false, m_drawer, m_drawerLayout));

    m_kiwiSdrApplet = new KiwiSdrApplet;
    m_appletOrder.append(makeEntry("KSDR", "KiwiSDR", m_kiwiSdrApplet, false,
                                   m_drawer, m_drawerLayout));

#ifdef HAVE_RADE
    m_radeApplet = new RadeApplet;
    m_appletOrder.append(makeEntry("RADE", "RADE Status", m_radeApplet, false, m_drawer, m_drawerLayout));
#endif

    m_healthApplet = new HealthApplet;
    m_appletOrder.append(makeEntry("HLTH", "Antenna Health", m_healthApplet, false, m_drawer, m_drawerLayout));

    m_agApplet = new AntennaGeniusApplet;
    {
        auto entry = makeEntry("AG", "Antenna Genius", m_agApplet, false,
                               m_drawer, m_drawerLayout);
        m_agBtn = entry.btn;
        markHardwareConditional("AG");
        m_appletOrder.append(entry);
    }

    // ShackSwitch applet — shown instead of/alongside AG for ShackSwitch devices.
    // Button starts hidden (mirroring AG) and is shown only when a ShackSwitch
    // is actually connected or discovered, so a fresh install with no SS
    // configured does not display a stray SS button in the top right.
    m_ssApplet = new ShackSwitchApplet;
    {
        auto entry = makeEntry("SS", "ShackSwitch", m_ssApplet, false, m_drawer, m_drawerLayout);
        m_ssBtn = entry.btn;
        markHardwareConditional("SS");
        m_appletOrder.append(entry);
    }

#ifdef HAVE_MQTT
    m_mqttApplet = new MqttApplet;
    m_appletOrder.append(makeEntry("MQTT", "MQTT", m_mqttApplet, false, m_drawer, m_drawerLayout));
#endif

    // Place the drawer toggle into the favorites row, then apply the
    // saved (or default) favorites layout to populate both strips.
    loadButtonLayout();
    applyBarLayout();

    // Restore drawer open/closed state (default closed).  Signals blocked
    // to skip the AppSettings::save() roundtrip during init.
    const bool drawerOpen =
        AppSettings::instance().value("ButtonBarDrawerOpen", "False").toString() == "True";
    {
        QSignalBlocker b(m_drawerToggleBtn);
        m_drawerToggleBtn->setChecked(drawerOpen);
    }
    m_drawer->setVisible(drawerOpen);
    m_drawerToggleBtn->setText(drawerOpen
        ? QString::fromUtf8("\xe2\x96\xb2")
        : QString::fromUtf8("\xe2\x96\xbc"));

    // ── Restore saved order ─────────────────────────────────────────────────
    QString savedOrder = settings.value("AppletOrder").toString();
    if (!savedOrder.isEmpty()) {
        QStringList ids = savedOrder.split(',');
        QVector<AppletEntry> reordered;
        for (const auto& id : ids) {
            for (int i = 0; i < m_appletOrder.size(); ++i) {
                if (m_appletOrder[i].id == id) {
                    reordered.append(m_appletOrder[i]);
                    m_appletOrder.remove(i);
                    break;
                }
            }
        }

        // One-shot migration for saved layouts from before WAVE existed:
        // place WAVE immediately after EQ instead of letting the generic
        // "new applets" append path put it at the very end.
        bool migratedWaveOrder = false;
        if (ids.contains("EQ") && !ids.contains("WAVE")) {
            int waveIdx = -1;
            for (int i = 0; i < m_appletOrder.size(); ++i) {
                if (m_appletOrder[i].id == "WAVE") {
                    waveIdx = i;
                    break;
                }
            }
            if (waveIdx >= 0) {
                const AppletEntry waveEntry = m_appletOrder[waveIdx];
                m_appletOrder.remove(waveIdx);
                for (int i = 0; i < reordered.size(); ++i) {
                    if (reordered[i].id == "EQ") {
                        reordered.insert(i + 1, waveEntry);
                        migratedWaveOrder = true;
                        break;
                    }
                }
            }
        }

        // Append any remaining (new applets not in saved order)
        reordered.append(m_appletOrder);
        m_appletOrder = reordered;
        if (migratedWaveOrder)
            saveOrder();
    }

    rebuildStackOrder();

    // Float-state restore for individual applets happens per-entry in
    // makeEntry via the legacy FloatingApplet_<id>_IsFloating migration;
    // no separate loop needed.
}

void AppletPanel::rebuildStackOrder()
{
    // Remove all items from layout without reparenting (avoids visibility issues)
    while (m_stack->count() > 0) {
        auto* item = m_stack->takeAt(0);
        delete item;  // deletes the layout item, NOT the widget
    }
    // Re-add in current order (skip floating containers to avoid stealing them)
    for (const auto& entry : m_appletOrder) {
        if (auto* cw = qobject_cast<ContainerWidget*>(entry.widget); cw && cw->isFloating())
            continue;
        m_stack->addWidget(entry.widget);
    }
    m_stack->addStretch(1);  // factor 1: absorb all surplus, pin tiles to sizeHint (#3461)
}

void AppletPanel::saveOrder()
{
    QStringList ids;
    for (const auto& entry : m_appletOrder)
        ids.append(entry.id);
    AppSettings::instance().setValue("AppletOrder", ids.join(","));
    AppSettings::instance().save();
}

void AppletPanel::setAppletVisible(const QString& id, bool visible)
{
    for (const auto& entry : m_appletOrder) {
        if (entry.id != id) continue;
        if (auto* c = qobject_cast<ContainerWidget*>(entry.widget)) {
            c->setContainerVisible(visible);
        } else if (entry.widget) {
            entry.widget->setVisible(visible);
        }
        if (entry.btn) {
            QSignalBlocker b(entry.btn);
            entry.btn->setChecked(visible);
        }
        return;
    }

    // Fall through to the container manager — some applets
    // (CEQ, CMP, CHAIN since #1713 Phase 4b) live as sub-containers
    // inside a composite tile rather than as standalone AppletEntry
    // instances, so the legacy id lookup misses them.  Try case-
    // insensitively since MainWindow calls use upper-case while the
    // container IDs are lower-case.
    if (m_containerMgr) {
        if (auto* c = m_containerMgr->container(id.toLower())) {
            c->setContainerVisible(visible);
        }
    }
}

void AppletPanel::setPooDooActiveSide(PooDooSide side)
{
    if (!m_containerMgr) return;
    // Containers tagged as TX-only or RX-only.  CEQ has dedicated
    // tiles per side ("ceq" = TX, "ceq-rx" = RX), as do GATE / CMP /
    // TUBE / PUDU once Phases 7.2-7.5 ship (their RX siblings will
    // join this list as they're added).  DESS and REVERB are TX-only
    // — they hide entirely on RX and reappear on TX.
    static const QStringList kTxOnly{
        QStringLiteral("ceq"),
        QStringLiteral("dess"),
        QStringLiteral("gate"),
        QStringLiteral("cmp"),
        QStringLiteral("tube"),
        QStringLiteral("pudu"),
        QStringLiteral("reverb"),
    };
    static const QStringList kRxOnly{
        QStringLiteral("ceq-rx"),
        QStringLiteral("gate-rx"),
        QStringLiteral("cmp-rx"),
        QStringLiteral("tube-rx"),
        QStringLiteral("pudu-rx"),
        QStringLiteral("dsp-rx"),
    };
    const bool txActive = (side == PooDooSide::Tx);

    auto applyVisibility = [this](const QStringList& ids, bool visible) {
        for (const auto& id : ids) {
            if (auto* c = m_containerMgr->container(id)) {
                // Force the underlying QWidget visibility too — insertChildWidget
                // calls show() directly which can leave m_visible=true while the
                // user's saved tab says hide.  setContainerVisible() short-circuits
                // when m_visible already matches, so call QWidget::setVisible
                // unconditionally to reconcile.
                c->setContainerVisible(visible);
                c->setVisible(visible);
            }
        }
    };
    applyVisibility(kTxOnly, txActive);
    applyVisibility(kRxOnly, !txActive);

    // If the parent tx_dsp container is currently floating, the set of
    // visible children just changed — its sizeHint shrank or grew but
    // the floating window won't refit on its own.  Defer one event-loop
    // tick so child layouts (e.g. ClientChainWidget, which dynamically
    // setFixedHeight()s itself based on row count) finish recalculating
    // before we read sizeHint.
    if (auto* parent = m_containerMgr->container("tx_dsp")) {
        if (parent->isFloating()) {
            if (QWidget* win = parent->window()) {
                QTimer::singleShot(0, win, [win]() {
                    win->adjustSize();
                });
            }
        }
    }
}

void AppletPanel::setScrollBarOnLeft(bool onLeft)
{
    if (!m_scrollArea) return;
    // Flip the QScrollArea's layout direction — vertical scroll bar moves
    // to the opposite edge.  Force the inner content widget back to LTR
    // so the applets themselves don't mirror.
    m_scrollArea->setLayoutDirection(onLeft ? Qt::RightToLeft : Qt::LeftToRight);
    if (auto* content = m_scrollArea->widget())
        content->setLayoutDirection(Qt::LeftToRight);
}

void AppletPanel::resetOrder()
{
    // Reorder m_appletOrder to match kDefaultOrder
    QVector<AppletEntry> reordered;
    for (const auto& id : kDefaultOrder) {
        for (int i = 0; i < m_appletOrder.size(); ++i) {
            if (m_appletOrder[i].id == id) {
                reordered.append(m_appletOrder[i]);
                m_appletOrder.remove(i);
                break;
            }
        }
    }
    reordered.append(m_appletOrder);
    m_appletOrder = reordered;
    rebuildStackOrder();
    AppSettings::instance().remove("AppletOrder");
    AppSettings::instance().save();
}

int AppletPanel::dropIndexFromY(int localY) const
{
    int idx = 0;
    for (int i = 0; i < m_appletOrder.size(); ++i) {
        auto* w = m_appletOrder[i].widget;
        if (!w) continue;
        if (auto* cw = qobject_cast<ContainerWidget*>(w); cw && cw->isFloating()) continue;
        if (!w->isVisible()) continue;
        int mid = w->mapTo(m_scrollArea->widget(), QPoint(0, w->height() / 2)).y();
        if (localY > mid) idx = i + 1;
    }
    return idx;
}

// Conditional-presence setters for hardware-dependent tiles (tuner,
// amplifier, Antenna Genius).  MainWindow calls these when the
// corresponding device is discovered / lost; the tray button shows
// and the container becomes available.  Float state is restored
// automatically by the one-shot legacy migration in makeEntry.

// Mark a bar button as hardware-available / unavailable and propagate
// the change to the applet's checked state.  Show/hide of the button
// itself is left to applyBarLayout() so the combined (hardware-available
// ∧ ¬user-hidden) decision lives in one place.
void AppletPanel::updateHardwareAvailability(const QString& id,
                                             const QString& appletKey,
                                             bool hardwareVisible)
{
    for (auto& bb : m_barButtons) {
        if (bb.id != id) continue;
        if (!bb.btn) return;
        const bool wasAvailable = bb.hardwareAvailable;
        bb.hardwareAvailable = hardwareVisible;
        if (hardwareVisible) {
            // First-time hardware-detect for a button the user hasn't
            // placed yet → append to Active so the picker shows it and
            // the bar can render it.  If the user has explicitly hidden
            // it (m_hiddenButtons), respect that.
            if (!wasAvailable
                && !m_buttonOrder.contains(id)
                && !m_hiddenButtons.contains(id)) {
                m_buttonOrder.append(id);
                saveButtonLayout();
            }
            const bool savedOn =
                AppSettings::instance().value(appletKey, "True").toString() == "True";
            if (savedOn && !bb.btn->isChecked()) bb.btn->setChecked(true);
        } else {
            // Preserve the saved checked state (so a later reconnect
            // restores the user's preference) — flip the live button
            // off without rewriting Applet_<id>.
            if (bb.btn->isChecked()) {
                QSignalBlocker block(bb.btn);
                bb.btn->setChecked(false);
            }
        }
        return;
    }
}

void AppletPanel::setTunerVisible(bool visible)
{
    updateHardwareAvailability("TUN", "Applet_TUN", visible);
    applyBarLayout();
}

void AppletPanel::setAmpVisible(bool visible)
{
    updateHardwareAvailability("AMP", "Applet_AMP", visible);
    applyBarLayout();
}

void AppletPanel::setAgVisible(bool visible)
{
    updateHardwareAvailability("AG", "Applet_AG", visible);
    applyBarLayout();
}

void AppletPanel::setShackSwitchVisible(bool visible)
{
    updateHardwareAvailability("SS", "Applet_SS", visible);
    applyBarLayout();
}

bool AppletPanel::controlsLocked() const
{
    return ControlsLock::isLocked();
}

void AppletPanel::setControlsLocked(bool locked)
{
    ControlsLock::setLocked(locked);
    m_lockBtn->setText("LOCK");
    m_lockBtn->setChecked(locked);
}

void AppletPanel::setSlice(SliceModel* slice)
{
    m_rxApplet->setSlice(slice);

    if (slice) {
        connect(slice, &SliceModel::modeChanged,
                m_phoneCwApplet, &PhoneCwApplet::setMode);
        m_phoneCwApplet->setMode(slice->mode());
    }
}

void AppletPanel::setAntennaList(const QStringList& ants)
{
    m_rxApplet->setAntennaList(ants);
}

void AppletPanel::setMaxSlices(int maxSlices)
{
    m_rxApplet->setMaxSlices(maxSlices);
}

void AppletPanel::clearSliceButtons()
{
    m_rxApplet->clearSliceButtons();
}

void AppletPanel::updateSliceButtons(const QList<SliceModel*>& slices, int activeSliceId)
{
    m_rxApplet->updateSliceButtons(slices, activeSliceId);
}

void AppletPanel::setRxDspChainOrder(
    const QVector<AudioEngine::RxChainStage>& stages)
{
    if (!m_containerMgr) return;
    auto* parent = m_containerMgr->container("tx_dsp");
    if (!parent) return;

    auto idFor = [](AudioEngine::RxChainStage s) -> QString {
        switch (s) {
            case AudioEngine::RxChainStage::Eq:    return "ceq-rx";
            case AudioEngine::RxChainStage::Gate:  return "gate-rx";
            case AudioEngine::RxChainStage::Comp:  return "cmp-rx";
            case AudioEngine::RxChainStage::Tube:  return "tube-rx";
            case AudioEngine::RxChainStage::Pudu:  return "pudu-rx";
            case AudioEngine::RxChainStage::DeEss: return {};  // no RX applet yet
            case AudioEngine::RxChainStage::None:  return {};
        }
        return {};
    };

    for (auto s : stages) {
        const QString id = idFor(s);
        if (id.isEmpty()) continue;
        auto* child = m_containerMgr->container(id);
        if (!child) continue;
        parent->removeChildWidget(child);
        parent->insertChildWidget(-1, child);
    }
}

void AppletPanel::setTxDspChainOrder(
    const QVector<AudioEngine::TxChainStage>& stages)
{
    if (!m_containerMgr) return;
    auto* parent = m_containerMgr->container("tx_dsp");
    if (!parent) return;

    // Map enum → child-container id.  Stages not present in the chain
    // are ignored (they stay wherever they currently sit; their tiles
    // should have been hidden via stageEnabledChanged anyway).
    auto idFor = [](AudioEngine::TxChainStage s) -> QString {
        switch (s) {
            case AudioEngine::TxChainStage::Gate:   return "gate";
            case AudioEngine::TxChainStage::Eq:     return "ceq";
            case AudioEngine::TxChainStage::DeEss:  return "dess";
            case AudioEngine::TxChainStage::Comp:   return "cmp";
            case AudioEngine::TxChainStage::Tube:   return "tube";
            case AudioEngine::TxChainStage::Enh:    return "pudu";
            case AudioEngine::TxChainStage::Reverb: return "reverb";
            case AudioEngine::TxChainStage::None:   return {};
        }
        return {};
    };

    // Pluck each ordered child out of its current position and re-
    // insert at the end.  Walking the list in chain order rebuilds the
    // parent body in the desired order without touching CHAIN itself —
    // CHAIN lives at index 0 and isn't in the ordered set, so it stays
    // put as the earlier siblings migrate to the end around it.
    for (auto s : stages) {
        const QString id = idFor(s);
        if (id.isEmpty()) continue;
        auto* child = m_containerMgr->container(id);
        if (!child) continue;
        parent->removeChildWidget(child);
        parent->insertChildWidget(-1, child);
    }
}

bool AppletPanel::eventFilter(QObject* obj, QEvent* ev)
{
    if (m_scrollArea && obj == m_scrollArea->verticalScrollBar()) {
        switch (ev->type()) {
        case QEvent::Enter:
        case QEvent::MouseButtonPress:
            if (m_scrollDimTimer) m_scrollDimTimer->stop();
            setScrollHandleActive(true);
            break;
        case QEvent::Leave:
        case QEvent::MouseButtonRelease:
            if (m_scrollDimTimer) m_scrollDimTimer->start();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void AppletPanel::setScrollHandleActive(bool active)
{
    if (!m_scrollArea) return;
    auto* sb = m_scrollArea->verticalScrollBar();
    if (!sb) return;
    if (sb->property("active").toBool() == active) return;
    sb->setProperty("active", active);
    sb->style()->unpolish(sb);
    sb->style()->polish(sb);
}

// ── Button-bar (active + drawer + hidden) ──────────────────────────────────

void AppletPanel::registerBarButton(const QString& id, const QString& label,
                                    const QString& tooltip, QPushButton* btn)
{
    if (!btn) return;
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setFixedHeight(20);
    btn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(btn, &QWidget::customContextMenuRequested, this,
            [this](const QPoint&) { openFavoritesPicker(); });
    m_barButtons.append(BarButton{id, label, tooltip, btn, /*hardwareAvailable=*/true});
}

QStringList AppletPanel::defaultButtonOrder() const
{
    // Top kFavoriteCount entries become the bar favourites.  Remaining
    // ids are in canonical registration order so a fresh install matches
    // the old fixed two-row bar.  IDs are the canonical persistence
    // keys, which differ from a few labels (WAVE→"WAV", PHNE→"PHN",
    // TXDSP→"VUDU").
    //
    // TUN/AMP/AG are intentionally omitted — these are hardware-
    // conditional and should not clutter the picker's Active column
    // until the matching device is detected.  updateHardwareAvailability()
    // auto-adds them when MainWindow reports the hardware as present,
    // and the user's explicit Hidden choice is respected from then on.
    QStringList out = {"VU", "RX", "TX", "P/CW", "WAVE"};
    const QStringList rest = {
        "LCK", "PHNE", "EQ", "TXDSP",
        "CAT", "DAX", "TCI", "IQ", "MTR", "PROF", "SS", "MQTT"
    };
    for (const auto& id : rest)
        if (!out.contains(id)) out.append(id);
    return out;
}

void AppletPanel::loadButtonLayout()
{
    m_buttonOrder.clear();
    m_hiddenButtons.clear();

    // New canonical key: ButtonBarLayout = {"order":[...], "hidden":[...]}.
    const QString rawLayout =
        AppSettings::instance().value("ButtonBarLayout").toString();
    if (!rawLayout.isEmpty()) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(rawLayout.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject obj = doc.object();
            for (const auto& v : obj.value("order").toArray())
                if (v.isString()) m_buttonOrder.append(v.toString());
            for (const auto& v : obj.value("hidden").toArray())
                if (v.isString()) m_hiddenButtons.insert(v.toString());
            return;
        }
    }

    // Migration: legacy ButtonBarFavorites (5-item array of canonical
    // favourite IDs) → ButtonBarLayout.  We adopt the saved favourites
    // as the head of the order list; applyBarLayout()'s sanitize pass
    // fills in the rest from defaultButtonOrder() and persists.
    const QString rawFavs =
        AppSettings::instance().value("ButtonBarFavorites").toString();
    if (!rawFavs.isEmpty()) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(rawFavs.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isArray()) {
            for (const auto& v : doc.array())
                if (v.isString()) m_buttonOrder.append(v.toString());
        }
        AppSettings::instance().remove("ButtonBarFavorites");
        return;
    }

    // First launch — use defaults.
    m_buttonOrder = defaultButtonOrder();
}

void AppletPanel::saveButtonLayout()
{
    QJsonArray orderArr;
    for (const auto& id : m_buttonOrder) orderArr.append(id);
    // QSet has no guaranteed iteration order; the hidden list doesn't
    // need ordering since hidden buttons aren't displayed.
    QJsonArray hiddenArr;
    for (const auto& id : m_hiddenButtons) hiddenArr.append(id);
    QJsonObject obj;
    obj["order"] = orderArr;
    obj["hidden"] = hiddenArr;
    const QString json = QString::fromUtf8(
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
    AppSettings::instance().setValue("ButtonBarLayout", json);
    AppSettings::instance().save();
}

void AppletPanel::markHardwareConditional(const QString& id)
{
    // Mark a bar button as hardware-conditional: it defaults to invisible
    // in the bar and won't be auto-appended to Active by applyBarLayout()'s
    // sanitize pass.  When MainWindow later confirms the hardware is
    // present, updateHardwareAvailability() flips hardwareAvailable=true
    // and adds the button to Active.
    for (auto& bb : m_barButtons) {
        if (bb.id != id) continue;
        bb.hardwareAvailable = false;
        if (bb.btn) bb.btn->hide();
        return;
    }
}

void AppletPanel::disableAppletForButton(const QString& id)
{
    for (const auto& bb : m_barButtons) {
        if (bb.id != id) continue;
        if (bb.btn && bb.btn->isChecked()) {
            // Triggers the existing toggled handler — sets
            // Applet_<id>=False and hides the container.  Works for
            // LCK/VU via their own handlers too.
            bb.btn->setChecked(false);
        }
        break;
    }
}

void AppletPanel::applyBarLayout()
{
    if (!m_favLayout || !m_drawerLayout || !m_drawerToggleBtn) return;

    // Batch all reparent + addWidget calls so Qt does a single layout
    // pass and style polish at the end.  Without this, ~19 individual
    // setParent() calls each trigger a cascading invalidation — visible
    // as a startup delay when MainWindow's hardware-presence setters
    // fire applyBarLayout() several times in a row.
    const bool favWasUpdating = m_favRow->updatesEnabled();
    const bool drawerWasUpdating = m_drawer->updatesEnabled();
    m_favRow->setUpdatesEnabled(false);
    m_drawer->setUpdatesEnabled(false);
    auto restoreUpdates = qScopeGuard([&] {
        m_favRow->setUpdatesEnabled(favWasUpdating);
        m_drawer->setUpdatesEnabled(drawerWasUpdating);
    });

    // Sanitize: drop unknown IDs from both lists, then append any
    // newly-registered buttons (e.g. a freshly added applet in a future
    // build) to the active list so they appear.
    {
        QSet<QString> registered;
        for (const auto& bb : m_barButtons) registered.insert(bb.id);

        QStringList validOrder;
        for (const auto& id : m_buttonOrder)
            if (registered.contains(id) && !validOrder.contains(id))
                validOrder.append(id);

        QSet<QString> validHidden;
        for (const auto& id : m_hiddenButtons)
            if (registered.contains(id)) validHidden.insert(id);

        // Buttons known to the build but missing from both lists →
        // default to Active (visible).  Skip hardware-conditional
        // buttons (TUN/AMP/AG/SS) until the corresponding device is
        // detected — updateHardwareAvailability() adds them on first
        // hardware-present transition.
        for (const auto& bb : m_barButtons) {
            if (validOrder.contains(bb.id) || validHidden.contains(bb.id)) continue;
            if (!bb.hardwareAvailable) continue;
            validOrder.append(bb.id);
        }

        if (validOrder != m_buttonOrder || validHidden != m_hiddenButtons) {
            m_buttonOrder = validOrder;
            m_hiddenButtons = validHidden;
            saveButtonLayout();
        }
    }

    auto detach = [](QLayout* layout, QWidget* widget) {
        if (!widget || !layout) return;
        layout->removeWidget(widget);
    };

    // Detach every registered bar button + the drawer toggle from
    // whichever container they're in.  They keep their parent (we
    // re-parent below) so signal connections survive.
    for (const auto& bb : m_barButtons) {
        if (auto* l = bb.btn->parentWidget() ? bb.btn->parentWidget()->layout() : nullptr)
            detach(l, bb.btn);
    }
    if (auto* l = m_drawerToggleBtn->parentWidget()
                      ? m_drawerToggleBtn->parentWidget()->layout()
                      : nullptr) {
        detach(l, m_drawerToggleBtn);
    }

    // Both the favorites strip and the drawer use the same column
    // count so cell widths match exactly across the two grids.
    constexpr int kCols = 6;

    auto findButton = [this](const QString& id) -> BarButton* {
        for (auto& bb : m_barButtons)
            if (bb.id == id) return &bb;
        return nullptr;
    };

    // Walk m_buttonOrder, placing each visible button first into the
    // favourites row (positions 0..kFavoriteCount-1) and then into the
    // drawer grid.  Hidden buttons and hardware-unavailable buttons get
    // parked + hidden — they consume no slot.
    int favSlot = 0;
    int drawerIdx = 0;
    for (const auto& id : m_buttonOrder) {
        BarButton* bb = findButton(id);
        if (!bb || !bb->btn) continue;

        const bool wantsVisible =
            bb->hardwareAvailable && !m_hiddenButtons.contains(bb->id);
        if (!wantsVisible) {
            bb->btn->setParent(m_drawer);
            bb->btn->hide();
            continue;
        }

        if (favSlot < kFavoriteCount) {
            bb->btn->setParent(m_favRow);
            m_favLayout->addWidget(bb->btn, 0, favSlot);
            bb->btn->setVisible(true);
            ++favSlot;
        } else {
            bb->btn->setParent(m_drawer);
            m_drawerLayout->addWidget(bb->btn, drawerIdx / kCols, drawerIdx % kCols);
            bb->btn->setVisible(true);
            ++drawerIdx;
        }
    }

    // User-hidden buttons (not in m_buttonOrder) — defensively park them.
    for (auto& bb : m_barButtons) {
        if (!m_hiddenButtons.contains(bb.id)) continue;
        bb.btn->setParent(m_drawer);
        bb.btn->hide();
    }

    // Equal-stretch on both grids so favourite cell width tracks drawer
    // cell width even when the favourites row isn't fully populated.
    for (int c = 0; c < kCols; ++c) {
        m_favLayout->setColumnStretch(c, 1);
        m_drawerLayout->setColumnStretch(c, 1);
    }

    // Drawer-toggle is always the last slot of the favorites row.
    m_drawerToggleBtn->setParent(m_favRow);
    m_favLayout->addWidget(m_drawerToggleBtn, 0, kCols - 1);
    m_drawerToggleBtn->show();
}

void AppletPanel::setDrawerOpen(bool open)
{
    if (!m_drawer || !m_drawerToggleBtn) return;
    m_drawer->setVisible(open);
    // Full-size triangle: ▼ closed, ▲ open
    m_drawerToggleBtn->setText(open
        ? QString::fromUtf8("\xe2\x96\xb2")
        : QString::fromUtf8("\xe2\x96\xbc"));
    AppSettings::instance().setValue(
        "ButtonBarDrawerOpen", open ? "True" : "False");
    AppSettings::instance().save();
}

void AppletPanel::openFavoritesPicker()
{
    // Lazy-construct + raise pattern per docs/style/dialog-patterns.md.
    // The dialog is non-modal, WA_DeleteOnClose'd, and frameless-aware
    // via its PersistentDialog base.  Subsequent right-clicks while the
    // picker is open just raise it.
    if (!m_favoritesPicker) {
        QList<FavoritesPickerDialog::Entry> entries;
        for (const auto& bb : m_barButtons)
            entries.append({bb.id, bb.label, bb.tooltip});

        const QStringList hiddenList(m_hiddenButtons.cbegin(),
                                     m_hiddenButtons.cend());

        auto* dlg = new FavoritesPickerDialog(
            entries, m_buttonOrder, hiddenList, kFavoriteCount, window());
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &FavoritesPickerDialog::layoutAccepted, this,
                [this](const QStringList& activeOrder,
                       const QStringList& hidden) {
                    const QSet<QString> newHidden(hidden.cbegin(),
                                                  hidden.cend());
                    // Newly-hidden buttons: disable their applets so a
                    // hidden tile doesn't keep occupying screen real
                    // estate after the user hides its toggle.
                    for (const auto& id : newHidden) {
                        if (m_hiddenButtons.contains(id)) continue;
                        disableAppletForButton(id);
                    }
                    m_buttonOrder = activeOrder;
                    m_hiddenButtons = newHidden;
                    saveButtonLayout();
                    applyBarLayout();
                });
        m_favoritesPicker = dlg;
    }
    m_favoritesPicker->show();
    m_favoritesPicker->raise();
    m_favoritesPicker->activateWindow();
}

} // namespace AetherSDR
