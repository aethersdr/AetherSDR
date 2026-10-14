#include "core/DroopCalibration.h"
#include "RadioSetupDialog.h"
#include "FirmwareReleasesDialog.h"
#include "RtlReceiverSettingsWidget.h"
#include "SerialPortCombo.h"
#include "models/CwDecodeSettings.h"
#include "RttyDecodeSettings.h"
#include "ScopedChildWidget.h"
#include "GuardedSlider.h"
#include "MixerControlAvailability.h"
#include "ComboStyle.h"
#include "DocsLinks.h"
#include "SliceColorManager.h"
#include "models/RadioModel.h"
#include "models/XvtrPolicy.h"
#include "core/AppSettings.h"
#include "core/AutomationBridgeSettings.h"
#include "core/backends/hl2/Hl2Discovery.h"   // HL2 custom-nickname settings key
#include "core/backends/hl2/Hl2FreqCal.h"     // manual frequency calibration (Calibration page)
#include "core/NetworkSettings.h"
#include "core/backends/flex/PanadapterStream.h"
#include "core/KiwiSdrManager.h"
#include "KiwiPublicReceiverPicker.h"
#include "core/LogManager.h"
#include "core/PeripheralSettings.h"
#include <QApplication>
#include <QHash>
#include <QPair>
#include <QAbstractItemView>
#include <QLocale>
#include <QSysInfo>
#include "core/AudioEngine.h"
#ifdef HAVE_SERIALPORT
#include "core/SerialPortController.h"
#include "core/FlexControlManager.h"
#include <QSerialPortInfo>
#endif
#include "core/FirmwareUploader.h"
#include "core/FirmwareStager.h"
#include "core/TgxlConnection.h"
#include "core/PgxlConnection.h"
#include "core/AcomConnection.h"
#include "core/LpMeterConnection.h"
#include "core/SpeConnection.h"
#include "core/VkampConnection.h"
#include "core/backends/flex/WanConnection.h"   // PinnedCertInfo + WanCertCache (#2951)
#include "core/CallsignLookupService.h"
#include "core/QrzLookupSettings.h"
#include "core/UlanziDialMappings.h"
#include "models/AntennaGeniusModel.h"
#include "PeripheralAuthStore.h"
#include "core/PeripheralRemovalGuard.h"
#include "PeripheralAuthConnectFlow.h"

#include <QCloseEvent>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSpacerItem>
#include <QGroupBox>
#include <QFrame>
#include <QHeaderView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPushButton>
#include <QSlider>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QDoubleValidator>
#include <QTimer>
#include <QVector>
#include <QDesktopServices>
#include <QUrl>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QStandardPaths>
#include <QFileInfo>
#include <QMessageBox>
#include <QColorDialog>
#include <QRadioButton>
#include <QProgressBar>
#include <QProcess>
#include <QAccessible>
#include <QListWidget>
#include <QMenu>
#include <QStackedWidget>
#include <QStyle>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QScrollArea>
#include <QScrollBar>
#include <QPoint>
#include <QHostAddress>
#include <QClipboard>
#include <QDebug>
#include <QGuiApplication>
#include <QPainter>
#include <QRandomGenerator>
#include <QPaintEvent>
#include <QPointer>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QToolButton>
#include <QTreeWidget>
#include <QAction>
#include <QKeySequence>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <utility>
#include "core/ThemeManager.h"
#include "RadioSetupDialogCommon.h"
#include "CanonIndicators.h"
#include "FramelessResizer.h"
#include "ThemedImageCache.h"

namespace AetherSDR {

// Every radio-reported value label in this dialog comes from here so they are
// identical (#5507, #5857): the canon sheet styles the Value tone, and a
// disabled value takes the unavailable color with it.
static QLabel* makeValueLabel(const QString& text)
{
    auto* label = new QLabel(text);
    setTone(label, Tone::Value);
    return label;
}

static constexpr int kInfoLeftLabelWidth = 112;
// Room for the right column's longest caption ("Licensed version:") and no
// more, so its fields keep their width.
static constexpr int kInfoRightLabelWidth = 128;

static QString kiwiSetupApiPolicyText(KiwiSdrProtocol::ApiPolicy policy)
{
    switch (policy) {
    case KiwiSdrProtocol::ApiPolicy::Disabled:
        return QStringLiteral("API disabled");
    case KiwiSdrProtocol::ApiPolicy::Limited:
        return QStringLiteral("API limited");
    case KiwiSdrProtocol::ApiPolicy::Open:
        return QStringLiteral("API open");
    case KiwiSdrProtocol::ApiPolicy::Unknown:
        break;
    }
    return QStringLiteral("API unknown");
}

static QString kiwiSetupMetadataSummary(const KiwiSdrManager* manager,
                                        const QString& id)
{
    if (!manager || id.isEmpty()) {
        return QString();
    }

    const KiwiSdrProtocol::ReceiverMetadata metadata =
        manager->receiverMetadata(id);
    const KiwiSdrProtocol::ProtocolState protocol =
        manager->protocolState(id);
    QStringList parts;
    if (!metadata.serverVersion.isEmpty()) {
        parts << QStringLiteral("v%1").arg(metadata.serverVersion);
    }
    if (metadata.hasUsers && metadata.hasUsersMax) {
        parts << QStringLiteral("%1/%2 users")
                     .arg(metadata.users)
                     .arg(metadata.usersMax);
    } else if (metadata.hasUsers) {
        parts << QStringLiteral("%1 users").arg(metadata.users);
    }
    if (metadata.hasBusy && metadata.busy) {
        parts << QStringLiteral("busy");
    }
    if (metadata.hasCampStatus
        && metadata.campStatus != KiwiSdrProtocol::CampStatus::Unknown) {
        switch (metadata.campStatus) {
        case KiwiSdrProtocol::CampStatus::Offered:
            parts << QStringLiteral("monitor offered");
            break;
        case KiwiSdrProtocol::CampStatus::Queued:
            if (metadata.hasCampQueuePosition
                && metadata.hasCampQueueWaiters) {
                parts << QStringLiteral("queue %1/%2")
                             .arg(metadata.campQueuePosition)
                             .arg(metadata.campQueueWaiters);
            } else {
                parts << QStringLiteral("queued");
            }
            break;
        case KiwiSdrProtocol::CampStatus::Accepted:
            parts << (metadata.hasCampReceiverChannel
                          ? QStringLiteral("camping RX%1")
                                .arg(metadata.campReceiverChannel)
                          : QStringLiteral("camping"));
            break;
        case KiwiSdrProtocol::CampStatus::Rejected:
            parts << QStringLiteral("camp rejected");
            break;
        case KiwiSdrProtocol::CampStatus::AudioStopped:
            parts << QStringLiteral("camp audio stopped");
            break;
        case KiwiSdrProtocol::CampStatus::Disconnected:
            parts << QStringLiteral("camp disconnected");
            break;
        case KiwiSdrProtocol::CampStatus::Unknown:
            break;
        }
    }
    if (metadata.hasExtApi) {
        parts << QStringLiteral("%1 (%2)")
                     .arg(kiwiSetupApiPolicyText(metadata.apiPolicy))
                     .arg(metadata.extApi);
    }
    if (metadata.hasGpsGood) {
        parts << (metadata.gpsGood ? QStringLiteral("GPS good")
                                   : QStringLiteral("GPS not good"));
    }
    if (metadata.hasAdcClipping && metadata.adcClipping) {
        parts << QStringLiteral("ADC clipping");
    }
    if (metadata.hasCoverageCenter && metadata.hasCoverageBandwidth) {
        const double lowMhz =
            metadata.coverageCenterMhz - metadata.coverageBandwidthMhz * 0.5;
        const double highMhz =
            metadata.coverageCenterMhz + metadata.coverageBandwidthMhz * 0.5;
        parts << QStringLiteral("%1-%2 MHz")
                     .arg(lowMhz, 0, 'f', 3)
                     .arg(highMhz, 0, 'f', 3);
    }
    if (protocol.sound.observed
        && protocol.sound.lastObservedLayout
            != KiwiSdrProtocol::FrameLayout::Unknown) {
        parts << QStringLiteral("SND %1")
                     .arg(KiwiSdrProtocol::frameLayoutName(
                         protocol.sound.lastObservedLayout));
    }
    if (protocol.waterfall.observed
        && protocol.waterfall.lastObservedLayout
            != KiwiSdrProtocol::FrameLayout::Unknown) {
        parts << QStringLiteral("W/F %1")
                     .arg(KiwiSdrProtocol::frameLayoutName(
                         protocol.waterfall.lastObservedLayout));
    }
    return parts.join(QStringLiteral(" · "));
}

// Hide/show a navigation row only when its state changes; returns whether it
// did. Pair with settleNavigationLayout() once per handler. Re-hiding hidden
// rows on every status message kept a delayed layout pending; on macOS
// QAccessibleTableCell::state() then ran QTreeView::doItemsLayout() from inside
// the cell, whose TableModelChanged freed the running cell (Qt 6.8.3: SIGSEGV
// in QAbstractScrollArea::viewport()).
static bool setNavigationItemHidden(QTreeWidgetItem* item, bool hidden)
{
    if (!item || item->isHidden() == hidden) {
        return false;
    }
    item->setHidden(hidden);
    return true;
}

// Settle the navigation tree's layout on our own call stack once a handler has
// changed rows, so nothing is left pending for an accessibility query to run
// re-entrantly. One layout per handler, not one per row: Qt coalesced the
// delayed layouts before, and each synchronous layout tears down and rebuilds
// every accessible cell interface.
static void settleNavigationLayout(QTreeWidget* tree, bool changed)
{
    if (changed && tree) {
        tree->doItemsLayout();
    }
}

// Wrap a tab page in a vertical QScrollArea so tabs whose stacked groups exceed
// the dialog's visible height (Themes, Audio, Filters, Peripherals on small or
// high-DPI displays) get a vertical scrollbar instead of forcing the dialog
// past the screen edge (#3345). setWidgetResizable(true) keeps horizontal
// expansion intact and hides the scrollbar when content already fits — users
// on wide screens see no visual change.
// A push button takes the width its text needs, never its row's. Qt's default
// horizontal policy (Minimum) lets a button grow to fill a grid cell or a
// stretched row; this turns that default to Fixed on every button of a page,
// including buttons added later (a KiwiSDR row, a transverter tab), by
// re-running on each layout request. A button given any other policy, or a
// fixed width, is left alone.
class ButtonsFitContent : public QObject {
public:
    explicit ButtonsFitContent(QWidget* page) : QObject(page), m_page(page)
    {
        fit();
        page->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_page && event->type() == QEvent::LayoutRequest) {
            fit();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void fit()
    {
        fitButtons();
        // With buttons at their own width, a grid or a row with nothing left
        // that can grow would spread its spare width between its columns and
        // push related buttons apart. Give that width to the end instead, so
        // the contents pack to the left as they read. A layout that can grow
        // (a field, a slider), or that already has its own stretch, is left
        // as it was.
        packLayout(m_page->layout());
        for (QWidget* child : m_page->findChildren<QWidget*>()) {
            packLayout(child->layout());
        }
    }

    void packLayout(QLayout* layout)
    {
        if (!layout || layout->property("rsPacked").toBool()) {
            return;
        }
        for (int i = 0; i < layout->count(); ++i) {
            packLayout(layout->itemAt(i)->layout());
        }
        if (canGrowHorizontally(layout)) {
            return;
        }
        if (auto* grid = qobject_cast<QGridLayout*>(layout)) {
            for (int c = 0; c < grid->columnCount(); ++c) {
                if (grid->columnStretch(c) > 0) {
                    return;
                }
            }
            layout->setProperty("rsPacked", true);
            // Side-by-side caption: value pairs share the width: each value
            // column takes an equal part of it, so every pair starts its own
            // column across the page and a caption stays beside its value.
            if (isPairGrid(grid)) {
                for (int c = 1; c < grid->columnCount(); c += 2) {
                    grid->setColumnStretch(c, 1);
                }
                // A value keeps its own width at the left of its wider cell.
                for (int i = 0; i < grid->count(); ++i) {
                    QLayoutItem* item = grid->itemAt(i);
                    if (!qobject_cast<QLabel*>(item->widget())
                        && !(item->alignment() & Qt::AlignHorizontal_Mask)) {
                        item->setAlignment(item->alignment() | Qt::AlignLeft);
                    }
                }
                grid->invalidate();
                return;
            }
            // A real spacer, not a stretch on an empty column: QGridLayout
            // gives no width to a column with nothing in it.
            grid->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum),
                          0, grid->columnCount());
        } else if (auto* box = qobject_cast<QBoxLayout*>(layout)) {
            if (box->direction() != QBoxLayout::LeftToRight) {
                return;
            }
            for (int i = 0; i < box->count(); ++i) {
                if (box->stretch(i) > 0) {
                    return;
                }
            }
            layout->setProperty("rsPacked", true);
            box->addStretch(1);
        }
    }

    // Two or more caption: value pairs side by side: an even number of
    // columns, at least four, and at the start of every even column a caption
    // (a QLabel), nothing, or a value spanning the caption's place as well.
    static bool isPairGrid(QGridLayout* grid)
    {
        const int columns = grid->columnCount();
        if (columns < 4 || columns % 2 != 0) {
            return false;
        }
        for (int i = 0; i < grid->count(); ++i) {
            int row = 0, column = 0, rowSpan = 0, columnSpan = 0;
            grid->getItemPosition(i, &row, &column, &rowSpan, &columnSpan);
            QLayoutItem* item = grid->itemAt(i);
            if (column % 2 == 0 && columnSpan < 2 && !qobject_cast<QLabel*>(item->widget())) {
                return false;
            }
        }
        return true;
    }

    // Whether anything in `layout` really takes more width: an expanding
    // widget that is not held to one width (a field given setFixedWidth keeps
    // its Expanding policy), or an expanding spacer.
    static bool canGrowHorizontally(QLayout* layout)
    {
        for (int i = 0; i < layout->count(); ++i) {
            QLayoutItem* item = layout->itemAt(i);
            if (QWidget* w = item->widget()) {
                if (w->isHidden() || w->maximumWidth() <= w->minimumWidth()) {
                    continue;
                }
                // A plain container (a field with its buttons) grows when
                // what it holds does.
                if ((w->sizePolicy().expandingDirections() & Qt::Horizontal)
                    || (w->layout() && !qobject_cast<QGroupBox*>(w)
                        && canGrowHorizontally(w->layout()))) {
                    return true;
                }
            } else if (QLayout* inner = item->layout()) {
                if (canGrowHorizontally(inner)) {
                    return true;
                }
            } else if (item->expandingDirections() & Qt::Horizontal) {
                return true;
            }
        }
        return false;
    }

    void fitButtons()
    {
        for (QPushButton* button : m_page->findChildren<QPushButton*>()) {
            QSizePolicy policy = button->sizePolicy();
            if (policy.horizontalPolicy() == QSizePolicy::Minimum) {
                policy.setHorizontalPolicy(QSizePolicy::Fixed);
                button->setSizePolicy(policy);
                // A fixed button in a wider cell would center; it starts at
                // the cell's left edge like the field beside it would.
                if (QWidget* parent = button->parentWidget()) {
                    alignLeftIfUnset(parent->layout(), button);
                }
            }
        }
    }

    static bool alignLeftIfUnset(QLayout* layout, QWidget* widget)
    {
        if (!layout) {
            return false;
        }
        for (int i = 0; i < layout->count(); ++i) {
            QLayoutItem* item = layout->itemAt(i);
            if (item->widget() == widget) {
                if (!(item->alignment() & Qt::AlignHorizontal_Mask)) {
                    item->setAlignment(item->alignment() | Qt::AlignLeft);
                    layout->invalidate();
                }
                return true;
            }
            if (alignLeftIfUnset(item->layout(), widget)) {
                return true;
            }
        }
        return false;
    }

    QWidget* m_page;
};

static QWidget* wrapTabInScrollArea(QWidget* content)
{
    new ButtonsFitContent(content);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

static QString displayOrDash(const QString& value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("—") : trimmed;
}

static QString radioSerialNumber(const RadioModel* model)
{
    if (!model) {
        return QStringLiteral("—");
    }
    return displayOrDash(model->chassisSerial().isEmpty()
                             ? model->serial()
                             : model->chassisSerial());
}

static QString prefixedVersion(const QString& version)
{
    const QString trimmed = version.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("—");
    }
    if (trimmed.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        return trimmed;
    }
    return QStringLiteral("v%1").arg(trimmed);
}

static QString radioOptionsText(const RadioModel* model)
{
    if (!model) {
        return QStringLiteral("—");
    }

    QStringList options;
    for (const QString& rawOption : model->radioOptions().split(
             QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString option = rawOption.trimmed();
        if (option.compare(QLatin1String("GPS"), Qt::CaseInsensitive) != 0) {
            options.append(option);
        }
    }
    if (model->hasGpsSetupHardware()) {
        options.prepend(QStringLiteral("GPS"));
    }
    if (options.isEmpty() && model->amplifier().present()) {
        options.append(QStringLiteral("PGXL"));
    }
    return options.isEmpty() ? QStringLiteral("—") : options.join(QStringLiteral(", "));
}

static void showCopiedPopup(QWidget* anchor);

static constexpr int kCopyButtonSize = 20;

class CopyValueButton final : public QToolButton {
public:
    explicit CopyValueButton(QString fieldName,
                             std::function<QString()> valueProvider,
                             QWidget* parent = nullptr)
        : QToolButton(parent),
          m_fieldName(std::move(fieldName)),
          m_valueProvider(std::move(valueProvider))
    {
        setAutoRaise(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setFixedSize(kCopyButtonSize, kCopyButtonSize);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setStyleSheet(
            "QToolButton { background: transparent; border: 0; padding: 0; margin: 0; }"
            "QToolButton:focus { outline: none; }");
        resetToolTip();
        setAccessibleName(toolTip());

        connect(this, &QToolButton::clicked, this, [this] {
            if (!m_valueProvider) {
                return;
            }

            const QString text = m_valueProvider().trimmed();
            if (text.isEmpty() || text == QStringLiteral("—")) {
                return;
            }

            if (QClipboard* clipboard = QGuiApplication::clipboard()) {
                clipboard->setText(text);
            }

            showCopiedPopup(this);
        });
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const bool hasCopyableValue = [this] {
            if (!m_valueProvider) return false;
            const QString text = m_valueProvider().trimmed();
            return !text.isEmpty() && text != QStringLiteral("—");
        }();

        if (hasCopyableValue && (underMouse() || hasFocus())) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 16));
            painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 3, 3);
        }

        const auto& theme = ThemeManager::instance();
        QColor stroke = theme.color(QStringLiteral("color.canon.muted"));
        if (!hasCopyableValue) {
            stroke = theme.color(QStringLiteral("color.canon.line"));
        } else if (isDown()) {
            stroke = theme.color(QStringLiteral("color.canon.cyan"));
        } else if (underMouse() || hasFocus()) {
            stroke = theme.color(QStringLiteral("color.canon.ink"));
        }

        painter.setPen(QPen(stroke, 1.25));
        painter.setBrush(Qt::NoBrush);

        const qreal left = (width() - 16.0) / 2.0;
        const qreal top = (height() - 16.0) / 2.0;
        const QRectF back(left + 3.0, top + 1.5, 8.5, 11.0);
        const QRectF front(left + 5.5, top + 4.0, 8.5, 11.0);

        painter.drawRoundedRect(back, 1.5, 1.5);
        painter.fillRect(front.adjusted(0.8, 0.8, -0.8, -0.8), theme.color(QStringLiteral("color.canon.raised")));
        painter.drawRoundedRect(front, 1.5, 1.5);
    }

private:
    void resetToolTip()
    {
        setToolTip(QStringLiteral("Copy %1").arg(m_fieldName));
    }

    QString m_fieldName;
    std::function<QString()> m_valueProvider;
};

static QWidget* makeCopyableValueLabel(const QString& fieldName, QLabel* valueLabel)
{
    auto* wrapper = new QWidget;
    auto* layout = new QHBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    valueLabel->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);
    layout->addWidget(valueLabel);
    layout->addStretch(1);
    layout->addWidget(new CopyValueButton(fieldName, [valueLabel] {
        return valueLabel->text();
    }, wrapper));

    return wrapper;
}

static void showCopiedPopup(QWidget* anchor)
{
    if (!anchor) {
        return;
    }

    auto* popup = new QLabel(QStringLiteral("Copied"), nullptr,
                             Qt::ToolTip | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setAttribute(Qt::WA_ShowWithoutActivating);
    // A top-level popup, outside the window's canon sheet: canon tokens directly.
    AetherSDR::ThemeManager::instance().applyStyleSheet(popup, "QLabel { background: {{color.canon.raised}}; border: 1px solid {{color.canon.lineHi}};"
        " border-radius: 4px; color: {{color.canon.ink}}; font-size: 11px;"
        " font-weight: bold; padding: 4px 8px; }");
    popup->adjustSize();

    const QPoint globalCenter = anchor->mapToGlobal(anchor->rect().center());
    const QSize popupSize = popup->sizeHint();
    QPoint pos(globalCenter.x() - popupSize.width() / 2,
               globalCenter.y() - anchor->height() / 2 - popupSize.height() - 6);

    QRect screenRect;
    if (auto* screen = anchor->screen()) {
        screenRect = screen->availableGeometry();
    } else if (auto* primary = QGuiApplication::primaryScreen()) {
        screenRect = primary->availableGeometry();
    }
    if (screenRect.isValid()) {
        pos.setX(std::clamp(pos.x(), screenRect.left(),
                            screenRect.right() - popupSize.width()));
        if (pos.y() < screenRect.top()) {
            pos.setY(globalCenter.y() + anchor->height() / 2 + 6);
        }
    }

    popup->move(pos);
    popup->show();
    QTimer::singleShot(1000, popup, &QLabel::close);
}

// A status line that takes no room while it has nothing to say.
static void setShownText(QLabel* label, const QString& text)
{
    label->setText(text);
    label->setVisible(!text.isEmpty());
}

static QWidget* makeInfoField(const QString& labelText, QWidget* valueWidget,
                              int labelWidth = kInfoLeftLabelWidth)
{
    auto* wrapper = new QWidget;
    auto* layout = new QHBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* label = new QLabel(labelText);
    applyLabelStyle(label);
    label->setFixedWidth(labelWidth);
    layout->addWidget(label);
    if (qobject_cast<QAbstractButton*>(valueWidget) || valueWidget->property("rsChip").isValid()) {
        // A button or a status chip takes its text's width, at the left.
        layout->addWidget(valueWidget);
        layout->addStretch(1);
        return wrapper;
    }
    QSizePolicy policy = valueWidget->sizePolicy();
    if (policy.horizontalPolicy() != QSizePolicy::Fixed) {
        policy.setHorizontalPolicy(QSizePolicy::Expanding);
        valueWidget->setSizePolicy(policy);
    }
    layout->addWidget(valueWidget, 1);
    // The copy column of the fields above and below: a field ends where their
    // values do, rather than running under their copy buttons.
    layout->addSpacing(kCopyButtonSize);

    return wrapper;
}

static QWidget* makeCopyableInfoField(const QString& fieldName, const QString& labelText,
                                      QLabel* valueLabel, int labelWidth = kInfoLeftLabelWidth)
{
    auto* wrapper = new QWidget;
    auto* layout = new QHBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* label = new QLabel(labelText);
    applyLabelStyle(label);
    label->setFixedWidth(labelWidth);
    layout->addWidget(label);

    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    valueLabel->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);
    layout->addWidget(valueLabel, 1);
    layout->addWidget(new CopyValueButton(fieldName, [valueLabel] {
        return valueLabel->text();
    }, wrapper));

    return wrapper;
}

static constexpr const char* kSuppressAudioDeviceNotificationsKey =
    "SuppressAudioDeviceNotifications";

static QString normalizedOscillatorValue(QString value)
{
    value = value.trimmed().toLower();
    return value == "ext" ? QStringLiteral("external") : value;
}

static QString oscillatorSourceLabel(const QString& value)
{
    const QString normalized = normalizedOscillatorValue(value);
    if (normalized == "auto") return QStringLiteral("Auto");
    if (normalized == "external") return QStringLiteral("External 10 MHz");
    if (normalized == "gpsdo") return QStringLiteral("GPSDO");
    if (normalized == "tcxo") return QStringLiteral("TCXO");
    return value.trimmed().isEmpty() ? QStringLiteral("Unknown") : value.toUpper();
}

static QString oscillatorStatusText(const RadioModel* model)
{
    const QString setting = normalizedOscillatorValue(model->oscSetting());
    const QString state = normalizedOscillatorValue(model->oscState());
    if (state.isEmpty())
        return QStringLiteral("Waiting for oscillator status");

    QString text;
    if (setting == "auto" && state != "auto") {
        text = QStringLiteral("Auto -> %1").arg(oscillatorSourceLabel(state));
    } else if (!setting.isEmpty() && setting != state && state != "auto") {
        text = QStringLiteral("%1 -> %2")
                   .arg(oscillatorSourceLabel(setting), oscillatorSourceLabel(state));
    } else {
        text = oscillatorSourceLabel(state);
    }

    text += model->oscLocked() ? QStringLiteral(" Locked")
                               : QStringLiteral(" Unlocked");
    if (state == "external" && !model->extPresent())
        text += QStringLiteral(" (not detected)");
    return text;
}

static Tone oscillatorStatusTone(const RadioModel* model)
{
    if (normalizedOscillatorValue(model->oscState()).isEmpty())
        return Tone::Hint;
    return model->oscLocked() ? Tone::Ok : Tone::Error;
}

static void addOscillatorChoice(QComboBox* combo, const QString& label,
                                const QString& value)
{
    if (combo->findData(value) < 0)
        combo->addItem(label, value);
}

static void refreshOscillatorSourceCombo(QComboBox* combo, const RadioModel* model,
                                         const QString& preferred = {})
{
    const QString current = normalizedOscillatorValue(
        preferred.isEmpty() ? combo->currentData().toString() : preferred);
    const QString setting = normalizedOscillatorValue(model->oscSetting());
    const QString state = normalizedOscillatorValue(model->oscState());
    const bool hasOscillatorStatus = !state.isEmpty();

    auto shouldKeep = [&](const QString& value) {
        return current == value || setting == value || state == value;
    };

    combo->clear();
    addOscillatorChoice(combo, QStringLiteral("Auto"), QStringLiteral("auto"));
    if (hasOscillatorStatus || model->tcxoPresent() || shouldKeep(QStringLiteral("tcxo")))
        addOscillatorChoice(combo, QStringLiteral("TCXO"), QStringLiteral("tcxo"));
    if (model->gpsdoPresent() || shouldKeep(QStringLiteral("gpsdo")))
        addOscillatorChoice(combo, QStringLiteral("GPSDO"), QStringLiteral("gpsdo"));
    if (hasOscillatorStatus || model->extPresent() || shouldKeep(QStringLiteral("external")))
        addOscillatorChoice(combo, QStringLiteral("External 10 MHz"),
                            QStringLiteral("external"));

    const QString desired = setting.isEmpty() ? current : setting;
    int idx = combo->findData(desired);
    if (idx < 0) idx = combo->findData(current);
    if (idx < 0) idx = combo->findData(QStringLiteral("auto"));
    if (idx >= 0) combo->setCurrentIndex(idx);
}

// The style guide's canon vocabulary (RFC #6226), as Network Diagnostics uses
// it for the same search / navigation / page shape: transparent over
// CanonWindow's ground except the surfaces that carry content. The navigation
// list and the group boxes are cards, fields sit on control, and section
// titles and focus are cyan. Every page is styled here; widgets carry a role
// (RadioSetupDialogCommon.h) rather than a sheet of their own.
static constexpr const char* kRadioSetupStyle = R"(
QWidget {
    color: {{color.canon.inkSoft}};
    background: transparent;
}
QLabel {
    background: transparent;
}
QLabel:disabled,
QCheckBox:disabled,
QRadioButton:disabled {
    color: {{color.control.unavailable}};
}
QLabel[rsTone="caption"] {
    color: {{color.canon.inkSoft}};
    font-size: 12px;
}
QLabel[rsTone="value"] {
    color: {{color.canon.ink}};
    font-size: 12px;
    font-weight: 700;
}
QLabel[rsTone="hint"] {
    color: {{color.canon.muted}};
    font-size: 11px;
}
QLabel[rsTone="heading"] {
    color: {{color.canon.cyan}};
    font-size: 12px;
    font-weight: 700;
}
QLabel[rsTone="title"] {
    color: {{color.canon.ink}};
    font-size: 20px;
    font-weight: 700;
}
QLabel[rsTone="accent"] {
    color: {{color.canon.cyan}};
    font-size: 11px;
}
QLabel[rsTone="ok"] {
    color: {{color.accent.success}};
    font-size: 11px;
}
QLabel[rsTone="warn"] {
    color: {{color.accent.warning}};
    font-size: 11px;
}
QLabel[rsTone="error"] {
    color: {{color.accent.danger}};
    font-size: 11px;
}
QLabel[rsTone="value"]:disabled,
QLabel[rsTone="caption"]:disabled,
QLabel[rsTone="hint"]:disabled {
    color: {{color.control.unavailable}};
}
QLabel[rsChip="off"],
QLabel[rsChip="on"] {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    padding: 4px 10px;
    font-weight: 600;
}
QLabel[rsChip="on"] {
    color: {{color.canon.ink}};
    background: {{color.toggle.success.background.checked}};
    border-color: {{color.toggle.success.border.checked}};
}
QLineEdit#radioSetupSearch {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 8px;
    padding: 8px 11px;
    font-size: 13px;
}
QLineEdit#radioSetupSearch:focus {
    border-color: {{color.canon.aqua}};
}
QTreeWidget#radioSetupNavigation {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 12px;
    padding: 6px;
    outline: none;
}
QTreeWidget#radioSetupNavigation::branch {
    image: none;
    border-image: none;
    background: transparent;
}
QTreeWidget#radioSetupNavigation::branch:selected,
QTreeWidget#radioSetupNavigation::branch:hover {
    background: {{color.canon.nested}};
}
QTreeWidget#radioSetupNavigation::item {
    min-height: 28px;
    padding: 2px 9px;
    border-radius: 6px;
}
QTreeWidget#radioSetupNavigation::item:selected {
    color: {{color.canon.onAccent}};
    background: {{color.canon.cyan}};
}
QTreeWidget#radioSetupNavigation::item:hover:!selected {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
}
/* A group's title is a heading on its own line above the box, in line with
   the box's left edge and a few pixels clear of it, as a section heading
   (Add Receiver) is above its box; not a label cut into the top border. */
QGroupBox {
    color: {{color.canon.cyan}};
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.line}};
    border-radius: 8px;
    margin-top: 24px;
    padding-top: 0px;
    font-weight: 700;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 1px;
    top: 0px;
    padding: 0;
}
QGroupBox QGroupBox {
    background: {{color.canon.nested}};
}
QPushButton {
    color: {{color.canon.cyan}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    padding: 4px 10px;
    font-weight: 600;
}
QPushButton:hover {
    background: {{color.canon.nested}};
    color: {{color.canon.aqua}};
}
QPushButton:focus {
    border-color: {{color.canon.aqua}};
}
QPushButton:checked {
    color: {{color.canon.ink}};
    background: {{color.toggle.success.background.checked}};
    border-color: {{color.toggle.success.border.checked}};
}
QPushButton[rsButton="select"]:checked {
    color: {{color.canon.onAccent}};
    background: {{color.canon.cyan}};
    border-color: {{color.canon.cyan}};
}
QPushButton[rsButton="mute"]:checked,
QPushButton[rsButton="danger"] {
    color: {{color.accent.danger}};
    background: {{color.toggle.footer.danger.background.checked}};
    border-color: {{color.accent.danger}};
}
QPushButton[rsButton="danger"]:hover {
    background: {{color.toggle.footer.danger.background.hover}};
}
QPushButton[rsButton="success"] {
    color: {{color.accent.success}};
    background: {{color.toggle.footer.success.background.checked}};
    border-color: {{color.accent.success}};
}
QPushButton[rsButton="success"]:hover {
    background: {{color.toggle.footer.success.background.hover}};
}
/* Unavailable reads the same checked or not: an engaged Mute on a radio that
   cannot mute shows no engaged red (theme-style-guide.md, Three-state
   controls). */
QPushButton:disabled,
QPushButton:checked:disabled,
QPushButton[rsButton="mute"]:checked:disabled,
QPushButton[rsButton="select"]:checked:disabled,
QPushButton[rsButton="danger"]:disabled,
QPushButton[rsButton="success"]:disabled {
    color: {{color.control.unavailable}};
    border-color: {{color.canon.line}};
    background: transparent;
}
QLineEdit,
QSpinBox,
QDoubleSpinBox,
QComboBox {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    padding: 3px 6px;
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
}
QLineEdit:focus,
QSpinBox:focus,
QDoubleSpinBox:focus,
QComboBox:focus {
    border-color: {{color.canon.aqua}};
}
QLineEdit:disabled,
QSpinBox:disabled,
QDoubleSpinBox:disabled,
QComboBox:disabled {
    color: {{color.control.unavailable}};
    border-color: {{color.canon.line}};
}
QLineEdit[readOnly="true"] {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
}
QComboBox QAbstractItemView,
QMenu {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.lineHi}};
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
}
QMenu::item:selected {
    color: {{color.canon.onAccent}};
    background: {{color.canon.cyan}};
}
QMenu::item:disabled {
    color: {{color.control.unavailable}};
}
QCheckBox,
QRadioButton {
    background: transparent;
    color: {{color.canon.inkSoft}};
    spacing: 8px;
}
QSlider::groove:horizontal {
    background: {{color.canon.control}};
    height: 6px;
    border-radius: 3px;
}
QSlider::sub-page:horizontal {
    background: {{color.canon.cyan}};
    border-radius: 3px;
}
QSlider::handle:horizontal {
    background: {{color.canon.ink}};
    width: 14px;
    margin: -5px 0;
    border-radius: 7px;
}
QSlider::groove:horizontal:disabled,
QSlider::sub-page:horizontal:disabled {
    background: {{color.canon.nested}};
}
QSlider::handle:horizontal:disabled {
    background: {{color.control.unavailable}};
}
QProgressBar {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    text-align: center;
    font-size: 11px;
}
QProgressBar::chunk {
    background: {{color.canon.cyan}};
    border-radius: 3px;
}
QListWidget,
QTableWidget,
QPlainTextEdit,
QTextEdit {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 6px;
    selection-background-color: {{color.canon.control}};
    selection-color: {{color.canon.ink}};
}
QTableWidget {
    gridline-color: {{color.canon.line}};
}
QHeaderView::section {
    color: {{color.canon.cyan}};
    background: {{color.canon.raised}};
    border: none;
    border-bottom: 1px solid {{color.canon.line}};
    padding: 4px 6px;
    font-weight: 700;
}
QTabWidget::pane {
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.line}};
    border-radius: 8px;
    top: -1px;
}
QTabBar::tab {
    color: {{color.canon.muted}};
    background: transparent;
    border: none;
    border-bottom: 2px solid transparent;
    padding: 5px 12px;
    font-weight: 600;
}
QTabBar::tab:hover {
    color: {{color.canon.ink}};
}
QTabBar::tab:selected {
    color: {{color.canon.cyan}};
    border-bottom-color: {{color.canon.cyan}};
}
QFrame#kiwiAntennaRow {
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 6px;
}
QScrollArea,
QScrollArea > QWidget > QWidget,
QStackedWidget,
QSplitter::handle {
    background: transparent;
    border: none;
}
QScrollBar:vertical {
    background: transparent;
    width: 12px;
    margin: 8px 2px 8px 2px;
    border-radius: 6px;
}
QScrollBar::handle:vertical {
    background: {{color.canon.lineHi}};
    border-radius: 5px;
    min-height: 34px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0px;
}
QScrollBar:horizontal {
    background: transparent;
    height: 12px;
    margin: 2px 8px 2px 8px;
    border-radius: 6px;
}
QScrollBar::handle:horizontal {
    background: {{color.canon.lineHi}};
    border-radius: 5px;
    min-width: 34px;
}
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal {
    width: 0px;
}
)";

// A spin box with a styled frame draws its steppers from the sheet or not at
// all: without these rules Qt falls back to two specks. Each stepper is a
// column at the field's right with a painted triangle in the canon ink,
// regenerated per theme like the combo arrow (ComboStyle.h). Left out if an
// arrow could not be written, so the field keeps Qt's own.
static QString spinArrowRules()
{
    const QColor color = ThemeManager::instance().color(QStringLiteral("color.canon.inkSoft"));
    const QString key = color.name(QColor::HexRgb).remove(QLatin1Char('#'));
    auto arrow = [&](const QString& name, bool up) {
        return themedImagePath(QStringLiteral("spin-arrows"), name, key, QSize(8, 5), 1,
                               [color, up](QPainter& p) {
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            const QPointF upTri[] = {{0, 5}, {8, 5}, {4, 0}};
            const QPointF downTri[] = {{0, 0}, {8, 0}, {4, 5}};
            p.drawPolygon(up ? upTri : downTri, 3);
        });
    };
    const QString up = arrow(QStringLiteral("up"), true);
    const QString down = arrow(QStringLiteral("down"), false);
    if (up.isEmpty() || down.isEmpty()) {
        return {};
    }
    return QStringLiteral(
        "QSpinBox, QDoubleSpinBox { padding-right: 16px; }"
        "QSpinBox::up-button, QDoubleSpinBox::up-button,"
        " QSpinBox::down-button, QDoubleSpinBox::down-button {"
        " subcontrol-origin: border; width: 16px; border: none;"
        " border-left: 1px solid {{color.canon.line}}; background: transparent; }"
        "QSpinBox::up-button, QDoubleSpinBox::up-button {"
        " subcontrol-position: top right; border-top-right-radius: 4px; }"
        "QSpinBox::down-button, QDoubleSpinBox::down-button {"
        " subcontrol-position: bottom right; border-bottom-right-radius: 4px; }"
        "QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,"
        " QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {"
        " background: {{color.canon.nested}}; }"
        "QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {"
        " image: url(\"%1\"); width: 8px; height: 5px; }"
        "QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {"
        " image: url(\"%2\"); width: 8px; height: 5px; }"
        "QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled,"
        " QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled,"
        " QSpinBox::up-arrow:off, QDoubleSpinBox::up-arrow:off,"
        " QSpinBox::down-arrow:off, QDoubleSpinBox::down-arrow:off { image: none; }")
        .arg(up, down);
}


RadioSetupDialog::RadioSetupDialog(RadioModel* model, AudioEngine* audio,
                                   TgxlConnection* tgxl, PgxlConnection* pgxl,
                                   AntennaGeniusModel* ag,
                                   KiwiSdrManager* kiwiSdrManager,
                                   AcomConnection* acom,
                                   SpeConnection* spe,
                                   VkampConnection* vkamp,
                                   LpMeterConnection* lpMeter,
                                   Kpa1500Connection* kpa1500,
                                   QWidget* parent)
    : CanonWindow(QStringLiteral("Radio Setup"), parent, Kind::Workspace),
      m_model(model), m_audio(audio),
      m_tgxl(tgxl), m_pgxl(pgxl), m_ag(ag),
      m_kiwiSdrManager(kiwiSdrManager), m_acom(acom), m_spe(spe), m_vkamp(vkamp), m_lpMeter(lpMeter),
      m_kpa1500(kpa1500)
{
    theme::setContainer(this, QStringLiteral("canon/radioSetup"));
    // Size and position persist under the key it used as a PersistentDialog,
    // so saved geometry carries over; it resizes from every edge. 40 px taller
    // than before for the canon header.
    setGeometryKey(QStringLiteral("RadioSetupDialogGeometry"));
    // The width is fixed: the pages are laid out for it. The height is the
    // operator's, from 720 up.
    setFixedWidth(960);
    setMinimumHeight(720);
    FramelessResizer::install(this);
    bodyWidget()->setObjectName(QStringLiteral("radioSetupBody"));
    applyCanonSheet(bodyWidget(), [] {
        return QString::fromLatin1(kRadioSetupStyle) + spinArrowRules() + canonIndicatorRules();
    });

    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setContentsMargins(22, 18, 22, 20);
    layout->setSpacing(10);
    layout->addWidget(makeCanonHeader(QStringLiteral("Radio Setup")));

    auto* search = new QLineEdit;
    search->setObjectName(QStringLiteral("radioSetupSearch"));
    search->setPlaceholderText(QStringLiteral("Search settings (%1)")
        .arg(QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText)));
    search->setClearButtonEnabled(true);
    search->setAccessibleName(QStringLiteral("Search Radio Setup settings"));
    search->setAccessibleDescription(
        QStringLiteral("Type a feature, device, or setting name to filter the navigation list."));
    search->setMinimumHeight(38);
    layout->addWidget(search);

    auto* content = new QSplitter(Qt::Horizontal);
    content->setChildrenCollapsible(false);

    m_navigation = new QTreeWidget;
    m_navigation->setObjectName(QStringLiteral("radioSetupNavigation"));
    m_navigation->setHeaderHidden(true);
    m_navigation->setRootIsDecorated(false);
    m_navigation->setIndentation(14);
    m_navigation->setUniformRowHeights(false);
    m_navigation->setMinimumWidth(235);
    m_navigation->setMaximumWidth(340);
    m_navigation->setAccessibleName(QStringLiteral("Radio Setup categories"));
    m_navigation->setAccessibleDescription(
        QStringLiteral("Use the arrow keys to move between categories and settings pages."));
    content->addWidget(m_navigation);

    auto* pageHost = new QWidget;
    auto* pageLayout = new QVBoxLayout(pageHost);
    pageLayout->setContentsMargins(16, 4, 4, 4);
    pageLayout->setSpacing(10);
    m_pageTitle = new QLabel;
    m_pageTitle->setAccessibleName(QStringLiteral("Current settings page"));
    setTone(m_pageTitle, Tone::Title);
    pageLayout->addWidget(m_pageTitle);
    m_pages = new QStackedWidget;
    m_pages->setObjectName(QStringLiteral("radioSetupPages"));
    m_pages->setAccessibleName(QStringLiteral("Settings page content"));
    pageLayout->addWidget(m_pages, 1);
    content->addWidget(pageHost);
    content->setStretchFactor(0, 0);
    content->setStretchFactor(1, 1);
    content->setSizes({260, 700});

    auto addCategory = [this](const QString& name) {
        auto* item = new QTreeWidgetItem(m_navigation, {name});
        item->setFlags(Qt::ItemIsEnabled);
        QFont font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
        return item;
    };

    auto* radioCategory = addCategory(QStringLiteral("RADIO"));
    auto* signalCategory = addCategory(QStringLiteral("RECEIVE & TRANSMIT"));
    auto* hardwareCategory = addCategory(QStringLiteral("CONTROLLERS & HARDWARE"));
    auto* onlineCategory = addCategory(QStringLiteral("ONLINE & APPEARANCE"));

    auto addPage = [this](QTreeWidgetItem* category, const QString& name,
                         const QString& keywords, std::function<QWidget*()> builder,
                         bool eager = false) {
        QWidget* placeholder = eager ? wrapTabInScrollArea(builder()) : new QWidget;
        const int index = m_pages->addWidget(placeholder);
        auto* item = new QTreeWidgetItem(category, {name});
        item->setData(0, Qt::UserRole, index);
        item->setData(0, Qt::UserRole + 1, keywords);
        item->setToolTip(0, keywords);
        m_pageIndexes.insert(name, index);
        m_pageItems.insert(index, item);
        if (!eager) {
            m_deferredBuilders[index] = std::move(builder);
        }
        return item;
    };

    // Build only the default (Radio) tab eagerly; defer the rest until first
    // selected.  This avoids hardware-probing calls (QSerialPortInfo,
    // QMediaDevices) during construction, which crash on some Wayland/Qt 6.11
    // configurations (#1776).
    QTreeWidgetItem* firstItem = addPage(radioCategory, QStringLiteral("Radio"),
        QStringLiteral("identity nickname callsign firmware license model region remote power"),
        [this] { return buildRadioTab(); }, true);
    addPage(radioCategory, QStringLiteral("Network"),
        QStringLiteral("ip address dhcp static ethernet connection network"),
        [this] { return buildNetworkTab(); });
    addPage(radioCategory, QStringLiteral("GPS"),
        QStringLiteral("gpsdo satellite location oscillator time"), [this] { return buildGpsTab(); });
    m_gpsPageIndex = m_pageIndexes.value(QStringLiteral("GPS"));
    addPage(signalCategory, QStringLiteral("Audio"),
        QStringLiteral("speaker microphone device sample rate latency sound dax"), [this] { return buildAudioTab(); });
    addPage(signalCategory, QStringLiteral("Transmit"),
        QStringLiteral("tx transmit rf power tune ptt mic override band settings"), [this] { return buildTxTab(); });
    addPage(signalCategory, QStringLiteral("Phone & CW"),
        QStringLiteral("phone cw keyer break-in sidetone microphone voice"), [this] { return buildPhoneCwTab(); });
    addPage(signalCategory, QStringLiteral("Receive"),
        QStringLiteral("rx receive calibration rf gain preamp"), [this] { return buildRxTab(); });
    addPage(signalCategory, QStringLiteral("Filters"),
        QStringLiteral("filter bandwidth low high cut mode"), [this] { return buildFiltersTab(); });
    m_filtersPageIndex = m_pageIndexes.value(QStringLiteral("Filters"));
    // Calibration page (HL2 and any future family that cannot calibrate itself).
    // Gated on the CAPABILITY, not on the family name: "does this radio correct
    // its own oscillator" is the question, and a Flex answers it on the Receive
    // page with its own hardware calibration.
    QTreeWidgetItem* calItem = addPage(radioCategory, QStringLiteral("Calibration"),
        QStringLiteral("frequency calibration ppb ppm oscillator crystal clock error wwv gpsdo zero beat"),
        [this] { return buildCalibrationTab(); });
    m_calibrationPageIndex = m_pageIndexes.value(QStringLiteral("Calibration"));
    setNavigationItemHidden(calItem, !m_model->backendCapabilities().hostFrequencyCalibration);
    connect(m_model, &RadioModel::connectionStateChanged, this, [this, calItem] {
        settleNavigationLayout(m_navigation, setNavigationItemHidden(
            calItem, !m_model->backendCapabilities().hostFrequencyCalibration));
        // A different radio may now be connected — re-read its own calibration
        // so a later Trim press cannot commit the previous radio's number.
        if (m_calibrationReseed)
            m_calibrationReseed();
    });
    addPage(radioCategory, QStringLiteral("RTL Receiver"),
        QStringLiteral("rtl receiver ppm oscillator frequency correction calibration iq dc suppression serial"),
        [this] { return new RtlReceiverSettingsWidget(*m_model); });
    m_rtlReceiverPageIndex = m_pageIndexes.value(QStringLiteral("RTL Receiver"));
    setNavigationItemHidden(m_pageItems.value(m_rtlReceiverPageIndex),
        !isCapabilityPageAvailable(m_pageItems.value(m_rtlReceiverPageIndex)));
    // HL2 Hardware page: which board variant is connected. Gated on the
    // backend declaring the hw.get/hw.set extension
    // (capabilities().extensionNamespaces), not a family string (#5554), like
    // the Calibration page.
    QTreeWidgetItem* hwItem = addPage(radioCategory, QStringLiteral("HL2 Hardware"),
        QStringLiteral("hermes lite hl2 squaresdr square sdr codec ak4951 dither band volts "
                       "speaker random filter board n2adr hpf cl1 reference clock gpsdo atu tuner"),
        [this] { return buildHl2HardwareTab(); });
    m_hl2HardwarePageIndex = m_pageIndexes.value(QStringLiteral("HL2 Hardware"));
    setNavigationItemHidden(hwItem, !declaresHl2Extension());
    connect(m_model, &RadioModel::connectionStateChanged, this, [this, hwItem] {
        settleNavigationLayout(m_navigation,
                               setNavigationItemHidden(hwItem, !declaresHl2Extension()));
        // A different HL2 may now be connected — re-read ITS options, or the
        // next click would write the previous radio's board into this one.
        if (m_hl2HardwareReseed)
            m_hl2HardwareReseed();
    });
    // Droop Correction page — mirrors the Calibration page immediately above:
    // gated on the CAPABILITY (RadioCapabilities::hostDroopCalibration, the
    // ANAN-G2 today), with the ANAN namespace enforced at the request boundary.
    QTreeWidgetItem* droopItem = addPage(radioCategory, QStringLiteral("Droop Correction"),
        QStringLiteral("droop calibration ddc0 panadapter spectrum sweep decimation edge cic"),
        [this] { return buildDroopCalibrationTab(); });
    m_droopCalibrationPageIndex = m_pageIndexes.value(QStringLiteral("Droop Correction"));
    setNavigationItemHidden(droopItem, !droopCalibrationAvailable(m_model->backend()));
    connect(m_model, &RadioModel::connectionStateChanged, this, [this, droopItem] {
        settleNavigationLayout(m_navigation, setNavigationItemHidden(
            droopItem, !droopCalibrationAvailable(m_model->backend())));
        if (m_droopReseed)
            m_droopReseed();
    });
    addPage(hardwareCategory, QStringLiteral("Antennas"),
        QStringLiteral("antenna names ant1 ant2 rx in transverter"), [this] { return buildAntennaNamesTab(); });
    addPage(hardwareCategory, QStringLiteral("Transverters"),
        QStringLiteral("xvtr transverter if frequency offset power"), [this] { return buildXvtrTab(); });
    // External APD tab (#2186) — only present on radios that report
    // `apd configurable=1` (FLEX-8x00 series with SmartSDR 4.2.18+).
    QTreeWidgetItem* apdItem = addPage(hardwareCategory, QStringLiteral("APD"),
        QStringLiteral("adaptive predistortion amplifier sampler linearization"), [this] { return buildApdTab(); });
    m_apdPageIndex = m_pageIndexes.value(QStringLiteral("APD"));
    setNavigationItemHidden(apdItem, !m_model->transmitModel().apdConfigurable());
    connect(&m_model->transmitModel(), &TransmitModel::apdStateChanged,
            this, [this, apdItem] {
        settleNavigationLayout(m_navigation, setNavigationItemHidden(
            apdItem, !m_model->transmitModel().apdConfigurable()));
    });
    addPage(hardwareCategory, QStringLiteral("USB Cables"),
        QStringLiteral("usb cable gpio bit bcd amplifier tuner accessory"), [this] { return buildUsbCablesTab(); });
    addPage(hardwareCategory, QStringLiteral("Peripherals"),
        QStringLiteral("controllers amplifier tuner antenna genius pgxl tgxl manual ip"), [this] { return buildPeripheralsTab(); });
    addPage(onlineCategory, QStringLiteral("Appearance & Behavior"),
        QStringLiteral("themes colors display font vision contrast click wheel ui enhancements"), [this] { return buildUiEnhancementsTab(); });
    addPage(onlineCategory, QStringLiteral("SmartLink"),
        QStringLiteral("remote internet certificate security pin wan"), [this] { return buildSmartLinkTab(); });
    m_smartLinkPageIndex = m_pageIndexes.value(QStringLiteral("SmartLink"));
    addPage(onlineCategory, QStringLiteral("QRZ & Callsigns"),
        QStringLiteral("qrz callsign lookup spots contact online account"), [this] { return buildQrzTab(); });
#ifdef HAVE_SERIALPORT
    addPage(hardwareCategory, QStringLiteral("Serial & Controllers"),
        QStringLiteral("serial flexcontrol midi controller knob com port baud ptt cw"), [this] { return buildSerialTab(); });
#endif

    m_navigation->expandAll();
    connect(m_navigation, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem* previous) {
        if (!current) {
            return;
        }
        if (!isCapabilityPageAvailable(current)) {
            selectTab(QStringLiteral("Radio"));
            return;
        }
        if (!current->parent()) {
            QTreeWidgetItem* next = previous && m_navigation->itemAbove(current) == previous
                ? m_navigation->itemBelow(current)
                : m_navigation->itemAbove(current);
            // Arrowing up onto the topmost "RADIO" header has nothing above it,
            // so itemAbove() is null and the highlight would rest on the header.
            // Clamp to the first child page below instead (#4183).
            if (!next || !next->parent()) {
                next = m_navigation->itemBelow(current);
            }
            if (next && next->parent()) {
                m_navigation->setCurrentItem(next);
            }
            return;
        }
        const int index = current->data(0, Qt::UserRole).toInt();
        buildDeferredTab(index);
        m_pages->setCurrentIndex(index);
        m_pageTitle->setText(current->text(0));
    });
    connect(search, &QLineEdit::textChanged, this, [this](const QString& text) {
        const QString needle = text.trimmed();
        QTreeWidgetItem* firstVisible = nullptr;
        bool navigationChanged = false;
        for (int i = 0; i < m_navigation->topLevelItemCount(); ++i) {
            QTreeWidgetItem* category = m_navigation->topLevelItem(i);
            bool anyVisible = false;
            for (int j = 0; j < category->childCount(); ++j) {
                QTreeWidgetItem* item = category->child(j);
                const QString haystack = item->text(0) + QStringLiteral(" ")
                    + item->data(0, Qt::UserRole + 1).toString();
                const bool matches = needle.isEmpty()
                    || haystack.contains(needle, Qt::CaseInsensitive);
                // Capability-gated rows must survive the filter: recomputing
                // setHidden() purely from the keyword match would unhide a page
                // the radio cannot use (typing "calibration" on a Flex would
                // surface the HL2-only page, whose controls move while nothing
                // reaches the radio).
                const bool apdRow = item == m_pageItems.value(m_apdPageIndex);
                const bool calRow = item == m_pageItems.value(m_calibrationPageIndex);
                const bool droopRow = item == m_pageItems.value(m_droopCalibrationPageIndex);
                const bool hl2HwRow = item == m_pageItems.value(m_hl2HardwarePageIndex);
                const bool gated =
                    ((isFlexOnlyPage(item) || item == m_pageItems.value(m_rtlReceiverPageIndex))
                        && !isCapabilityPageAvailable(item))
                    || (isGpsPage(item)
                        && !isGpsSetupAvailable())
                    || (apdRow && !m_model->transmitModel().apdConfigurable())
                    || (calRow && !m_model->backendCapabilities().hostFrequencyCalibration)
                    || (droopRow && !droopCalibrationAvailable(m_model->backend()))
                    || (hl2HwRow && !declaresHl2Extension());
                if (!gated) {
                    navigationChanged |= setNavigationItemHidden(item, !matches);
                }
                anyVisible = anyVisible || !item->isHidden();
                if (!item->isHidden() && !firstVisible) {
                    firstVisible = item;
                }
            }
            navigationChanged |= setNavigationItemHidden(category, !anyVisible);
            category->setExpanded(true);
        }
        settleNavigationLayout(m_navigation, navigationChanged);
        // Stash the first match but do NOT make it current here: selecting it
        // fires currentItemChanged → buildDeferredTab, which would eagerly
        // construct and hardware-probe deferred pages (Audio, Serial,
        // Peripherals) on every keystroke — the probe-on-navigate deferral
        // #1776 exists to avoid. Enter commits the highlight instead (#4183).
        // With an empty needle every page "matches", so leave the stash null —
        // Enter with no query typed is then a no-op rather than jumping to (and
        // building) the first page.
        m_searchFirstMatch = needle.isEmpty() ? nullptr : firstVisible;
    });
    connect(search, &QLineEdit::returnPressed, this, [this] {
        if (m_searchFirstMatch && !m_searchFirstMatch->isHidden()) {
            m_navigation->setCurrentItem(m_searchFirstMatch);
        }
    });
    auto* findAction = new QAction(this);
    findAction->setShortcut(QKeySequence::Find);
    findAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(findAction, &QAction::triggered, search, [search] {
        search->setFocus();
        search->selectAll();
    });
    addAction(findAction);
    m_navigation->setCurrentItem(firstItem);
    connect(m_model, &RadioModel::capabilitiesChanged, this,
            [this](bool, const RadioCapabilities&) {
        updateRadioCapabilityVisibility();
    });
    connect(m_model, &RadioModel::connectionStateChanged, this,
            [this](bool) {
        updateRadioCapabilityVisibility();
    });
    connect(m_model, &RadioModel::oscillatorChanged, this,
            [this] {
        updateRadioCapabilityVisibility();
    });
    connect(m_model, &RadioModel::gpsStatusChanged, this,
            [this] {
        updateRadioCapabilityVisibility();
    });
    updateRadioCapabilityVisibility();
    layout->addWidget(content, 1);
}

void RadioSetupDialog::showEvent(QShowEvent* event)
{
    CanonWindow::showEvent(event);
    // This dialog is persistent and may have been hidden while the selected
    // radio or an optional Flex GPSDO changed. Re-evaluate every gated page and
    // group before presenting the cached widget tree again.
    updateRadioCapabilityVisibility();
    // The dialog is a showOrRaisePersistent singleton and pages are built once,
    // so anything that changed the stored calibration while it was closed (a
    // `freqcal` bridge call, a different radio) has to be re-read here.
    if (m_calibrationReseed)
        m_calibrationReseed();
    if (m_droopReseed)
        m_droopReseed();
    if (m_hl2HardwareReseed)
        m_hl2HardwareReseed();
    // Same reason, for the PC audio device lists: a headset connected or
    // removed while this dialog was hidden has to be picked up on show.
    if (m_audioDeviceReseed)
        m_audioDeviceReseed();
    // Same reasoning, for every serial-port combo on a page that has been
    // built: the platform's port list can have changed completely while the
    // dialog was hidden, and there is no hotplug signal to tell us.
    for (const auto& reseed : m_serialPortReseeds)
        reseed();
}

bool RadioSetupDialog::declaresHl2Extension() const
{
    // Ask what the backend declares (it answers hw.get / hw.set), the same check
    // the page's reseed makes. No backend → page hidden, matching the
    // Calibration and Droop pages.
    return m_model && m_model->backendDeclaresExtension(QStringLiteral("hl2"));
}

bool RadioSetupDialog::isFlexOnlyPage(const QTreeWidgetItem* item) const
{
    if (!item) {
        return false;
    }
    const int index = item->data(0, Qt::UserRole).toInt();
    return index == m_filtersPageIndex || index == m_smartLinkPageIndex;
}

bool RadioSetupDialog::isCapabilityPageAvailable(const QTreeWidgetItem* item) const
{
    if (!m_model || !item) {
        return false;
    }
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index == m_rtlReceiverPageIndex) {
        return m_model->backendDeclaresExtension(QStringLiteral("rtl"))
            && m_model->backendCapabilities().extensions.value(QStringLiteral("rtl")).toMap()
                .value(QStringLiteral("settingsVersion")).toInt() == 1;
    }
    if (index == m_droopCalibrationPageIndex) {
        return droopCalibrationAvailable(m_model->backend());
    }
    if (!m_model->isConnected()) {
        return true;
    }
    const RadioCapabilities caps = m_model->backendCapabilities();
    if (index == m_filtersPageIndex) {
        return caps.hasSharpFilters;
    }
    if (index == m_smartLinkPageIndex) {
        return caps.hasSmartLink;
    }
    return true;
}

bool RadioSetupDialog::isGpsSetupAvailable() const
{
    return m_model && (!m_model->isConnected() || m_model->hasGpsSetupHardware());
}

bool RadioSetupDialog::isGpsPage(const QTreeWidgetItem* item) const
{
    return item && item->data(0, Qt::UserRole).toInt() == m_gpsPageIndex;
}

void RadioSetupDialog::updateRadioCapabilityVisibility()
{
    const bool connected = m_model->isConnected();
    const RadioCapabilities caps = m_model->backendCapabilities();
    // M3b: these legacy info-field hides await their scoped migration.
    // This legacy radio/API status is distinct from the host knob settings.
    if (m_flexControlInfoField) {
        m_flexControlInfoField->setVisible(!connected || caps.hasFlexControlIntegration);
    }
    if (m_multiFlexInfoField) {
        m_multiFlexInfoField->setVisible(!connected || caps.hasMultiClientSessions);
    }
    if (m_remoteOnInfoField) {
        applyCapabilitySurfaceAvailability(
            m_remoteOnInfoField, connected, caps.hasRemoteOnControl,
            tr("Not supported by this radio"));
    }
    if (m_rebootInfoField) {
        applyCapabilitySurfaceAvailability(m_rebootInfoField, connected, caps.canReboot,
            tr("Not supported by this radio"));
    }
    if (m_licenseInfoGroup) {
        applyCapabilityClusterVisibility(m_licenseInfoGroup, connected, caps.hasLicenseInfo);
    }
    if (m_firmwareUpdateGroup) {
        applyCapabilityClusterVisibility(
            m_firmwareUpdateGroup, connected, caps.canUpgradeFirmware);
    }
    if (m_firmwareDisclaimer) {
        applyCapabilityClusterVisibility(
            m_firmwareDisclaimer, connected, caps.canUpgradeFirmware);
    }
    if (m_networkIdentityGroup) {
        applyCapabilityClusterVisibility(
            m_networkIdentityGroup, connected, caps.hasNetworkConfigurationReadback);
    }
    if (m_vitaReceiveBufferLabel) {
        applyCapabilitySurfaceAvailability(
            m_vitaReceiveBufferLabel, connected, caps.usesVita49Transport,
            tr("Not supported by this radio"));
    }
    if (m_vitaReceiveBufferControls) {
        applyCapabilitySurfaceAvailability(
            m_vitaReceiveBufferControls, connected, caps.usesVita49Transport,
            tr("Not supported by this radio"));
    }
    if (m_vitaReceiveBufferStatus) {
        applyCapabilitySurfaceAvailability(
            m_vitaReceiveBufferStatus, connected, caps.usesVita49Transport,
            tr("Not supported by this radio"));
    }
    if (m_networkMtuLabel) {
        applyCapabilitySurfaceAvailability(
            m_networkMtuLabel, connected, caps.usesVita49Transport,
            tr("Not supported by this radio"));
    }
    if (m_networkMtuControl) {
        applyCapabilitySurfaceAvailability(
            m_networkMtuControl, connected, caps.usesVita49Transport,
            tr("Not supported by this radio"));
    }
    if (m_privateIpPolicyLabel) {
        applyCapabilitySurfaceAvailability(
            m_privateIpPolicyLabel, connected, caps.hasPrivateIpConnectionPolicy,
            tr("Not supported by this radio"));
    }
    if (m_privateIpPolicyControl) {
        applyCapabilitySurfaceAvailability(
            m_privateIpPolicyControl, connected, caps.hasPrivateIpConnectionPolicy,
            tr("Not supported by this radio"));
    }
    if (m_ipDhcpButton) {
        const bool canConfigure = caps.hasClientNetworkConfig;
        const QString sessionKey = connected
            ? QStringLiteral("%1:%2").arg(m_model->family(), m_model->serial())
            : QString();
        const QString tip = canConfigure
            ? QString()
            : tr("Changing the radio's IP configuration is not supported by this radio.");
        const bool isStatic = m_model->hasStaticIp();
        applyIpConfigPresentation(
            m_ipConfigPresentation, sessionKey, canConfigure, isStatic,
            isStatic ? m_model->staticIp() : m_model->ip(),
            isStatic ? m_model->staticNetmask() : m_model->netmask(),
            isStatic ? m_model->staticGateway() : m_model->gateway(),
            m_ipDhcpButton, m_ipStaticButton, m_staticIpEdit,
            m_staticMaskEdit, m_staticGatewayEdit, m_ipApplyButton, tip);
    }
    if (m_audioCompressionGroup) {
        m_audioCompressionGroup->setVisible(!connected || caps.hasAudioCompression);
    }
    // The host serial knob settings remain available for every radio (#5778).
    // Only the legacy radio/API info field above follows radio capabilities.
    if (m_optionsLabel) {
        m_optionsLabel->setText(radioOptionsText(m_model));
    }

    const QLineEdit* search = findChild<QLineEdit*>(QStringLiteral("radioSetupSearch"));
    const QString needle = search ? search->text().trimmed() : QString();
    bool navigationChanged = false;
    for (const int index : {m_filtersPageIndex, m_smartLinkPageIndex, m_rtlReceiverPageIndex}) {
        if (QTreeWidgetItem* item = m_pageItems.value(index, nullptr)) {
            const QString haystack = item->text(0) + QStringLiteral(" ")
                + item->data(0, Qt::UserRole + 1).toString();
            navigationChanged |= setNavigationItemHidden(item, !isCapabilityPageAvailable(item)
                            || (!needle.isEmpty()
                                && !haystack.contains(needle, Qt::CaseInsensitive)));
        }
    }

    if (QTreeWidgetItem* gpsItem = m_pageItems.value(m_gpsPageIndex, nullptr)) {
        const QString haystack = gpsItem->text(0) + QStringLiteral(" ")
            + gpsItem->data(0, Qt::UserRole + 1).toString();
        navigationChanged |= setNavigationItemHidden(gpsItem, !isGpsSetupAvailable()
                           || (!needle.isEmpty()
                               && !haystack.contains(needle, Qt::CaseInsensitive)));
    }
    settleNavigationLayout(m_navigation, navigationChanged);

    const bool currentPageUnavailable = m_navigation
        && (!isCapabilityPageAvailable(m_navigation->currentItem())
            || (!isGpsSetupAvailable()
                && isGpsPage(m_navigation->currentItem())));
    if (currentPageUnavailable) {
        if (QLineEdit* mutableSearch = findChild<QLineEdit*>(
                QStringLiteral("radioSetupSearch"))) {
            mutableSearch->clear();
        }
        selectTab(QStringLiteral("Radio"));
    }
}

void RadioSetupDialog::showFirmwareReleases(const QStringList& releases)
{
    if (releases.isEmpty())
        return;

    // The release-notes template comes from the backend's capability record,
    // not from a family check here: a backend that publishes firmware says
    // where its notes live, and one that does not leaves the rows unlinked.
    QString notesTemplate;
    if (const auto& source = m_model->backendCapabilities().firmwareUpdateSource)
        notesTemplate = source->releaseNotesUrlTemplate;

    // Rebuilt on every check rather than re-raised: the list and the radio's
    // own release are baked in at construction, so an open dialog from an
    // earlier check would otherwise keep showing that check's answer.
    if (m_firmwareReleasesDialog)
        m_firmwareReleasesDialog->close();
    auto* dlg = new FirmwareReleasesDialog(
        releases, m_model->softwareVersion(), notesTemplate, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_firmwareReleasesDialog = dlg;
    m_firmwareReleasesDialog->show();
    m_firmwareReleasesDialog->raise();
    m_firmwareReleasesDialog->activateWindow();
}

bool RadioSetupDialog::confirmFirmwareClose()
{
    // A nested close/reject must not destroy the owner underneath this prompt.
    if (m_firmwareClosePromptOpen) {
        return false;
    }
    if (!m_uploader || !m_uploader->isUploading()) {
        return true;
    }
    const QPointer<RadioSetupDialog> self(this);
    const QPointer<FirmwareUploader> uploader(m_uploader);
    ScopedChildWidget<QMessageBox> boxOwner(
        QMessageBox::Warning, tr("Firmware Update In Progress"), QString(),
        QMessageBox::Ok | QMessageBox::Cancel, this);
    QMessageBox* box = boxOwner.get();
    box->setDefaultButton(QMessageBox::Cancel);
    box->setEscapeButton(QMessageBox::Cancel);
    const auto refreshPrompt = [uploader, box] {
        if (!uploader) {
            return;
        }
        switch (uploader->phase()) {
        case FirmwareUploader::Phase::Preparing:
            box->setText(tr("The firmware upload is being prepared. No image bytes have been sent."
                            "\n\nClose this window and cancel the attempt?"));
            break;
        case FirmwareUploader::Phase::Transferring:
            box->setText(tr("A firmware upload is in progress. Closing this window stops the transfer "
                            "and may leave the radio with an incomplete image. The update outcome "
                            "will remain unknown; reconnect before retrying.\n\nClose anyway?"));
            break;
        case FirmwareUploader::Phase::AwaitingConfirmation:
            box->setText(tr("Firmware bytes have left the local write buffer, but the radio has not "
                            "confirmed installation. Closing this window stops waiting for confirmation; "
                            "it does not undo the update.\n\nReconnect to check the firmware version "
                            "before retrying. Close anyway?"));
            break;
        case FirmwareUploader::Phase::Idle:
            box->setText(tr("The firmware upload attempt has ended. Close this window?"));
            break;
        }
    };
    refreshPrompt();
    // The upload can advance or finish while exec() runs its nested event loop.
    connect(uploader, &FirmwareUploader::progressChanged, box, refreshPrompt);
    connect(uploader, &FirmwareUploader::finished, box, refreshPrompt);
    m_firmwareClosePromptOpen = true;
    const int reply = box->exec();
    if (!self) {
        return false;
    }
    m_firmwareClosePromptOpen = false;
    if (!boxOwner || reply != QMessageBox::Ok) {
        return false;
    }
    if (uploader) {
        // Classify the CURRENT phase; it may have changed inside the prompt.
        // cancel() is a no-op if a terminal radio result already arrived.
        uploader->cancel();
    }
    return !self.isNull();
}

void RadioSetupDialog::done(int result)
{
    // QDialog routes Escape, reject() and accept() through done(), bypassing
    // closeEvent. Keep those paths behind the same confirmation without
    // redirecting reject() to close() (which recurses during Qt's close path).
    if (!m_peripheralRemovalPending && confirmFirmwareClose()) {
        // Escape is a close like the corner button: it keeps the
        // uncommitted Peripherals edits too.
        for (const auto& saver : m_peripheralRowSavers)
            saver();
        CanonWindow::done(result);
    }
}

void RadioSetupDialog::closeEvent(QCloseEvent* event)
{
    if (m_peripheralRemovalPending || !confirmFirmwareClose()) {
        event->ignore();
        return;
    }
    // Persist any uncommitted "user cleared IP" edits in the Peripherals
    // tab before the base class flushes geometry to AppSettings.
    for (const auto& saver : m_peripheralRowSavers)
        saver();
    CanonWindow::closeEvent(event);
}

// ── Radio tab ─────────────────────────────────────────────────────────────────

QWidget* RadioSetupDialog::buildRadioTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Toggle button style: green when on, red when off

    auto makeToggle = [](bool checked) {
        auto* btn = new QPushButton(checked ? "Enabled" : "Disabled");
        btn->setCheckable(true);
        btn->setChecked(checked);
        return btn;
    };

    // Radio Information group
    {
        auto* group = new QGroupBox("Radio Information");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);

        m_serialLabel = makeValueLabel(radioSerialNumber(m_model));
        grid->addWidget(makeCopyableInfoField(QStringLiteral("Radio Serial Number"),
                                              QStringLiteral("Serial:"),
                                              m_serialLabel),
                        0, 0);

        // displayOrDash, never a default: m_region is set only by Flex paths
        // (`info` reply, RadioDelta), so it is always empty on an HL2 and must
        // show the em-dash like every other empty value here (#5507).
        m_regionLabel = makeValueLabel(displayOrDash(m_model->region()));
        // A copyable status field like Serial:/HW Version: (selectable, with a
        // CopyValueButton), not a toggle-styled box, which read as pressable
        // (#5507).
        grid->addWidget(makeCopyableInfoField(QStringLiteral("Region"),
                                              QStringLiteral("Region:"),
                                              m_regionLabel, kInfoRightLabelWidth),
                        0, 1);

        m_hwVersionLabel = makeValueLabel(prefixedVersion(m_model->version()));
        grid->addWidget(makeCopyableInfoField(QStringLiteral("HW Version"),
                                              QStringLiteral("HW Version:"),
                                              m_hwVersionLabel),
                        1, 0);

        m_remoteOnBtn = makeToggle(m_model->remoteOnEnabled());
        connect(m_remoteOnBtn, &QPushButton::toggled, this, [this](bool on) {
            m_remoteOnBtn->setText(on ? "Enabled" : "Disabled");
            m_model->setRemoteOnEnabled(on);
        });
        m_remoteOnInfoField = makeInfoField(QStringLiteral("Remote On:"), m_remoteOnBtn,
                                            kInfoRightLabelWidth);
        grid->addWidget(m_remoteOnInfoField, 1, 1);

        m_optionsLabel = makeValueLabel(radioOptionsText(m_model));
        grid->addWidget(makeCopyableInfoField(QStringLiteral("Options"),
                                              QStringLiteral("Options:"),
                                              m_optionsLabel),
                        2, 0);

        // FlexControl support is a status, not a setting: it shows whether
        // AetherSDR currently holds control via the FlexRadio API, tracking
        // isConnected() live like rebootBtn above.
        auto* fcLbl = new QLabel;
        auto updateFcLbl = [fcLbl](bool connected) {
            fcLbl->setText(connected ? "Enabled" : "Disabled");
            setChip(fcLbl, connected ? Chip::On : Chip::Off);
        };
        updateFcLbl(m_model->isConnected());
        fcLbl->setAlignment(Qt::AlignCenter);
        connect(m_model, &RadioModel::connectionStateChanged, this, updateFcLbl);
        m_flexControlInfoField = makeInfoField(QStringLiteral("FlexControl:"), fcLbl,
                                               kInfoRightLabelWidth);
        grid->addWidget(m_flexControlInfoField, 2, 1);

        auto* mfBtn = makeToggle(m_model->multiFlexEnabled());
        connect(mfBtn, &QPushButton::toggled, this, [this, mfBtn](bool on) {
            mfBtn->setText(on ? "Enabled" : "Disabled");
            m_model->setMultiFlexEnabled(on);
        });
        m_multiFlexInfoField = makeInfoField(QStringLiteral("multiFLEX:"), mfBtn,
                                             kInfoRightLabelWidth);
        grid->addWidget(m_multiFlexInfoField, 3, 1);

        auto* rebootBtn = new QPushButton(QStringLiteral("Reboot Radio"));
        setButtonRole(rebootBtn, ButtonRole::Danger);
        // Only enable when actually connected; subscribe so disconnect/reconnect
        // disables/re-enables the button without the user having to reopen the
        // dialog. rebootRadio() also early-returns on disconnected, but the
        // disabled state makes the affordance discoverable rather than silent.
        // F3 (#4448): also gate on the backend supporting a client reboot — HL2
        // is RX-only with no reboot command, and offering it would send a
        // meaningless command and trigger a forced disconnect.
        rebootBtn->setEnabled(m_model->isConnected() && m_model->backendCapabilities().canReboot);
        connect(m_model, &RadioModel::connectionStateChanged, rebootBtn,
                [this, rebootBtn](bool connected) {
            rebootBtn->setEnabled(connected && m_model->backendCapabilities().canReboot);
        });
        connect(rebootBtn, &QPushButton::clicked, this, [this] {
            const bool wan = m_model->isWan();
            const QString body = wan
                ? QStringLiteral("Reboot the connected radio now?\n\n"
                                 "AetherSDR will disconnect. SmartLink/WAN sessions "
                                 "do not auto-reconnect today — you will need to "
                                 "reconnect manually once the radio finishes booting.")
                : QStringLiteral("Reboot the connected radio now?\n\n"
                                 "AetherSDR will disconnect and automatically reconnect "
                                 "once the radio finishes booting.");
            const QPointer<RadioSetupDialog> self(this);
            const QPointer<RadioModel> model(m_model);
            ScopedChildWidget<QMessageBox> boxOwner(
                QMessageBox::Warning, QStringLiteral("Reboot Radio"), body,
                QMessageBox::Ok | QMessageBox::Cancel, this);
            boxOwner.get()->setDefaultButton(QMessageBox::Cancel);
            const int ret = boxOwner.get()->exec();
            if (!self || !boxOwner || !model || self->m_model != model.data()
                || ret != QMessageBox::Ok) {
                return;
            }
            model->rebootRadio();
            self->close();
        });
        m_rebootInfoField = makeInfoField(QStringLiteral("Reboot:"), rebootBtn,
                                          kInfoLeftLabelWidth);
        grid->addWidget(m_rebootInfoField, 3, 0);

        connect(m_model, &RadioModel::infoChanged, this, [this] {
            if (m_serialLabel) {
                m_serialLabel->setText(radioSerialNumber(m_model));
            }
            if (m_hwVersionLabel) {
                m_hwVersionLabel->setText(prefixedVersion(m_model->version()));
            }
            // Region: was missing from this lambda — m_regionLabel had no
            // setText anywhere in the file, so it froze at construction while
            // its three neighbours here refreshed. RadioModel::disconnectFromRadio
            // clears m_region, so even on a Flex the label went on showing the
            // PREVIOUS radio's region after a disconnect. (#5507 item 3)
            if (m_regionLabel) {
                m_regionLabel->setText(displayOrDash(m_model->region()));
            }
            if (m_optionsLabel) {
                m_optionsLabel->setText(radioOptionsText(m_model));
            }
        });

        for (auto* lbl : group->findChildren<QLabel*>()) {
            if (!hasTone(lbl))
                applyLabelStyle(lbl);
        }

        vbox->addWidget(group);
    }

    // Radio Identification group
    {
        auto* group = new QGroupBox("Radio Identification");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);

        m_modelLabel = makeValueLabel(displayOrDash(m_model->model()));
        grid->addWidget(makeCopyableInfoField(QStringLiteral("Model"),
                                              QStringLiteral("Model:"),
                                              m_modelLabel),
                        0, 0);

        // Radio-owned nicknames may be empty. Other radios use the local store.
        QString initialNickname = m_model->nickname();
        {
            const RadioInfo info = m_model->lastRadioInfo();
            if (!hl2::Hl2Discovery::nicknameLivesOnRadio(info))
                initialNickname = hl2::Hl2Discovery::effectiveNickname(
                    info.family, info.serial,
                    info.model.isEmpty() ? m_model->name() : info.model);
        }
        m_nicknameEdit = new QLineEdit(initialNickname);
        grid->addWidget(makeInfoField(QStringLiteral("Nickname:"), m_nicknameEdit,
                                      kInfoRightLabelWidth),
                        0, 1);

        m_callsignEdit = new QLineEdit(m_model->callsign());
        // Named for the screen reader and for the automation bridge, which
        // resolves controls by objectName / class / accessibleName. Both of
        // these fields were anonymous QLineEdits among many, so neither could
        // be driven in a test nor announced to a screen reader.
        m_callsignEdit->setAccessibleName(tr("Station callsign"));
        m_callsignEdit->setAccessibleDescription(
            tr("Your callsign, used for PSK Reporter, WSPR and spotting"));
        m_nicknameEdit->setAccessibleName(tr("Radio nickname"));
        // Read from the start, not scrolled to the cursor at the end.
        m_nicknameEdit->setCursorPosition(0);
        m_callsignEdit->setCursorPosition(0);
        grid->addWidget(makeInfoField(QStringLiteral("Callsign:"), m_callsignEdit),
                        1, 0);

        connect(m_nicknameEdit, &QLineEdit::editingFinished, this, [this] {
            // Return on an untouched field must not write radio-persisted identity.
            if (!m_nicknameEdit->isModified()) {
                return;
            }
            const RadioInfo info = m_model->lastRadioInfo();
            // Only FlexRadio has an on-radio name store ("radio name" command).
            // Every other family (HL2, the sim demo, any future non-Flex backend)
            // has no wire to store a name, so the "radio name" command is a silent
            // no-op there. For those, persist the operator's nickname client-side,
            // keyed by the radio's stable serial, so discovery shows it in the
            // picker on the next sweep and RadioSetup reads it back on reopen.
            if (hl2::Hl2Discovery::nicknameLivesOnRadio(info)) {
                if (m_model->requestRadioNickname(m_nicknameEdit->text())
                    == ReceiveDispatch::Unsupported) {
                    m_nicknameEdit->setText(m_model->nickname());
                }
            } else {
                // Commits eagerly inside; don't rely on the shutdown save.
                hl2::Hl2Discovery::setNickname(info.family, info.serial,
                                               m_nicknameEdit->text());
            }
            m_nicknameEdit->setModified(false);
        });
        connect(m_callsignEdit, &QLineEdit::editingFinished, this, [this] {
            // Persist client-side on every family, then also write the radio's
            // copy where one exists (see RadioModel::callsign()). Send first,
            // persist second: setStationCallsign() emits callsignChanged and
            // callsign() prefers the radio's value, so on a Flex listeners
            // would otherwise restart on the old value.
            const QString entered = m_callsignEdit->text().trimmed().toUpper();
            // Empty is "no change", never "erase". Before the station-callsign
            // setting existed, blanking this field sent `radio callsign ` with
            // an empty argument and was otherwise harmless; now it would ALSO
            // wipe a persisted setting that PSK Reporter, the WSPR beacon and
            // QRZ lookup all read. A silent wipe of persisted state is not an
            // acceptable cost for a stray keystroke. (PR #4537 review.)
            if (entered.isEmpty()) {
                m_callsignEdit->setText(m_model->callsign());
                return;
            }
            if (m_model->usesFlexCommandPlane()) {
                m_model->sendCommand("radio callsign " + entered);
            }
            m_model->setStationCallsign(entered);
            m_callsignEdit->setText(entered);
        });

        connect(m_model, &RadioModel::infoChanged, this, [this] {
            if (m_modelLabel) {
                m_modelLabel->setText(displayOrDash(m_model->model()));
            }
            if (m_nicknameEdit && !m_nicknameEdit->isModified()
                && hl2::Hl2Discovery::nicknameLivesOnRadio(m_model->lastRadioInfo())) {
                m_nicknameEdit->setText(m_model->nickname());
            }
        });

        QString stationVal = AppSettings::instance().value("StationName", "").toString();
        auto* stationEdit = new QLineEdit(
            stationVal.isEmpty() ? QSysInfo::machineHostName() : stationVal);
        stationEdit->setCursorPosition(0);
        stationEdit->setToolTip("Identifies this client to other Multi-Flex stations.\n"
                                "Defaults to OS hostname if empty.");
        grid->addWidget(makeInfoField(QStringLiteral("Station Name:"), stationEdit,
                                      kInfoRightLabelWidth),
                        1, 1);
        connect(stationEdit, &QLineEdit::editingFinished, this, [this, stationEdit] {
            auto& s = AppSettings::instance();
            s.setValue("StationName", stationEdit->text());
            s.save();
            m_model->sendCommand("client station " + stationEdit->text());
        });

        for (auto* lbl : group->findChildren<QLabel*>()) {
            if (!hasTone(lbl))
                applyLabelStyle(lbl);
        }

        vbox->addWidget(group);
    }

    // License Info group (matches SmartSDR Radio Setup → License Info section)
    {
        auto* group = new QGroupBox("License Info");
        m_licenseInfoGroup = group;
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);

        // Row 0: Subscription | Expiration
        m_licSubscriptionLabel = makeValueLabel(
            m_model->licenseSubscription().isEmpty() ? "—" : m_model->licenseSubscription());
        grid->addWidget(makeCopyableInfoField(QStringLiteral("Subscription"),
                                              QStringLiteral("Subscription:"),
                                              m_licSubscriptionLabel),
                        0, 0);

        m_licExpirationLabel = makeValueLabel(
            m_model->licenseExpirationDate().isEmpty() ? "—" : m_model->licenseExpirationDate());
        grid->addWidget(makeCopyableInfoField(QStringLiteral("Expiration"),
                                              QStringLiteral("Expiration:"),
                                              m_licExpirationLabel,
                                              kInfoRightLabelWidth),
                        0, 1);

        // Row 1: Radio ID | Licensed version
        m_licRadioIdLabel = makeValueLabel(
            m_model->licenseRadioId().isEmpty() ? "—" : m_model->licenseRadioId());
        grid->addWidget(makeCopyableInfoField(QStringLiteral("Radio ID"),
                                              QStringLiteral("Radio ID:"),
                                              m_licRadioIdLabel),
                        1, 0);

        m_licMaxVersionLabel = makeValueLabel(
            m_model->licenseMaxVersion().isEmpty() ? "—" : m_model->licenseMaxVersion());
        grid->addWidget(makeCopyableInfoField(QStringLiteral("Licensed version"),
                                              QStringLiteral("Licensed version:"),
                                              m_licMaxVersionLabel,
                                              kInfoRightLabelWidth),
                        1, 1);

        for (auto* lbl : group->findChildren<QLabel*>()) {
            if (!hasTone(lbl))
                applyLabelStyle(lbl);
        }

        // Update labels live if license status arrives after dialog opens
        connect(m_model, &RadioModel::infoChanged, this, [this] {
            if (!m_model->licenseSubscription().isEmpty())
                m_licSubscriptionLabel->setText(m_model->licenseSubscription());
            if (!m_model->licenseExpirationDate().isEmpty())
                m_licExpirationLabel->setText(m_model->licenseExpirationDate());
            if (!m_model->licenseRadioId().isEmpty())
                m_licRadioIdLabel->setText(m_model->licenseRadioId());
            if (!m_model->licenseMaxVersion().isEmpty())
                m_licMaxVersionLabel->setText(m_model->licenseMaxVersion());
        });

        vbox->addWidget(group);
    }

    // Firmware Update group
    {
        auto* group = new QGroupBox("Firmware Update");
        m_firmwareUpdateGroup = group;
        auto* vlay = new QVBoxLayout(group);
        vlay->setSpacing(6);

        // Current version row
        // Laid out like the info fields above it, so its value lines up
        // with theirs; half the group's width, as theirs are.
        auto* infoRow = new QGridLayout;
        infoRow->setSpacing(6);
        infoRow->setColumnStretch(0, 1);
        infoRow->setColumnStretch(1, 1);
        auto* curFw = makeValueLabel(displayOrDash(m_model->softwareVersion()));
        infoRow->addWidget(makeCopyableInfoField(QStringLiteral("FW Version"),
                                                 QStringLiteral("FW Version:"), curFw), 0, 0);
        vlay->addLayout(infoRow);

        connect(m_model, &RadioModel::infoChanged, this, [this, curFw] {
            curFw->setText(displayOrDash(m_model->softwareVersion()));
        });

        // Progress bar
        m_fwProgress = new QProgressBar;
        m_fwProgress->setRange(0, 100);
        m_fwProgress->setValue(0);
        m_fwProgress->setTextVisible(true);
        m_fwProgress->setFixedHeight(20);
        m_fwProgress->hide();
        vlay->addWidget(m_fwProgress);

        // Status label (multi-line)
        m_fwStatusLabel = new QLabel("");
        setTone(m_fwStatusLabel, Tone::Hint);
        m_fwStatusLabel->setWordWrap(true);
        // Hidden until there is something to say, so an idle group has no
        // blank line between the version and the buttons.
        m_fwStatusLabel->hide();
        vlay->addWidget(m_fwStatusLabel);

        // Button row
        auto* btnRow = new QHBoxLayout;
        auto* checkBtn = new QPushButton("Check for Update");
        btnRow->addWidget(checkBtn);

        auto* browseBtn = new QPushButton("Select Installer...");
        btnRow->addWidget(browseBtn);

        m_fwUploadBtn = new QPushButton("Upload Firmware");
        m_fwUploadBtn->setEnabled(false);
        setButtonRole(m_fwUploadBtn, ButtonRole::Success);
        btnRow->addWidget(m_fwUploadBtn);
        btnRow->addStretch(1);
        vlay->addLayout(btnRow);

        auto* disclaimer = new QLabel(
            "⚠ CAUTION: Firmware update is currently a highly experimental feature. "
            "Use at your own risk. At this time we still recommend updating "
            "via the SmartSDR Windows application.");
        disclaimer->setWordWrap(true);
        m_firmwareDisclaimer = disclaimer;
        setTone(disclaimer, Tone::Warn);
        vlay->addWidget(disclaimer);

        // ── Stager wiring ─────────────────────────────────────────────
        m_stager = new FirmwareStager(this);

        connect(checkBtn, &QPushButton::clicked, this, [this, checkBtn] {
            checkBtn->setEnabled(false);
            setTone(m_fwStatusLabel, Tone::Hint);
            m_stager->checkForUpdate(m_model->softwareVersion());
        });

        connect(m_stager, &FirmwareStager::updateCheckComplete, this,
            [this, checkBtn](const QString& latest, bool avail,
                             const QStringList& published) {
            checkBtn->setEnabled(true);
            if (avail) {
                setTone(m_fwStatusLabel, Tone::Warn);
                setShownText(m_fwStatusLabel, QString(
                    "Update available: v%1").arg(latest));
            } else {
                setTone(m_fwStatusLabel, Tone::Ok);
                setShownText(m_fwStatusLabel, "Firmware is up to date (v" + latest + ").");
            }
            // The verdict is one line; the LIST is the answer. FlexRadio
            // publishes several lines at once, so "newer firmware exists" on
            // its own does not tell a v3 operator which release is theirs.
            showFirmwareReleases(published);
        });

        connect(m_stager, &FirmwareStager::updateCheckFailed, this,
            [this, checkBtn](const QString& err) {
            checkBtn->setEnabled(true);
            setTone(m_fwStatusLabel, Tone::Error);
            setShownText(m_fwStatusLabel, err);
        });

        connect(m_stager, &FirmwareStager::stageProgress, this,
            [this](int pct, const QString& status) {
            m_fwProgress->setValue(pct);
            setTone(m_fwStatusLabel, Tone::Hint);
            setShownText(m_fwStatusLabel, status);
        });

        connect(m_stager, &FirmwareStager::stageComplete, this,
            [this](const QString& path, const QString&) {
            m_fwFilePath = path;
            m_fwUploadBtn->setEnabled(true);
            setTone(m_fwStatusLabel, Tone::Ok);
        });

        connect(m_stager, &FirmwareStager::stageFailed, this,
            [this](const QString& err) {
            m_fwProgress->hide();
            setTone(m_fwStatusLabel, Tone::Error);
            setShownText(m_fwStatusLabel, err);
        });

        // ── Browse / select installer manually ────────────────────────
        // Accepts the SmartSDR installer the user has already downloaded
        // from FlexRadio (.msi for v4.2+, .exe for older releases) or a
        // pre-extracted .ssdr file. The stager auto-detects which.
        connect(browseBtn, &QPushButton::clicked, this, [this] {
            const QPointer<RadioSetupDialog> self(this);
            const QPointer<RadioModel> model(m_model);
            const QString path = QFileDialog::getOpenFileName(
                this, "Select SmartSDR Installer or Firmware File", QString(),
                "SmartSDR installer or firmware (*.msi *.exe *.ssdr);;"
                "MSI installer (*.msi);;"
                "EXE installer (*.exe);;"
                "Extracted firmware (*.ssdr);;"
                "All files (*)");
            if (!self || !model || self->m_model != model.data() || path.isEmpty()) {
                return;
            }

            m_fwFilePath.clear();
            m_fwUploadBtn->setEnabled(false);
            m_fwProgress->show();
            m_fwProgress->setValue(0);
            setTone(m_fwStatusLabel, Tone::Hint);
            setShownText(m_fwStatusLabel, "Preparing firmware from " + QFileInfo(path).fileName() + "...");

            // The stager emits stageProgress / stageComplete / stageFailed.
            // Existing slots wire to those (set above) so we just kick it off.
            m_stager->stageFromLocalFile(path,
                FirmwareStager::modelToFamily(m_model->model()));
        });

        // ── Upload ────────────────────────────────────────────────────
        connect(m_fwUploadBtn, &QPushButton::clicked, this, [this] {
            if (m_fwFilePath.isEmpty()) return;

            const QPointer<RadioSetupDialog> self(this);
            const QPointer<RadioModel> model(m_model);
            ScopedChildWidget<QMessageBox> boxOwner(
                QMessageBox::Warning, QStringLiteral("Firmware Update"),
                QString("Upload %1 to %2?\n\n"
                        "The radio will reboot after the update.\n"
                        "Do not disconnect during the upload.")
                    .arg(QFileInfo(m_fwFilePath).fileName(), m_model->model()),
                QMessageBox::Ok | QMessageBox::Cancel, this);
            boxOwner.get()->setDefaultButton(QMessageBox::Cancel);
            const int reply = boxOwner.get()->exec();
            if (!self || !boxOwner || !model || self->m_model != model.data()
                || reply != QMessageBox::Ok) {
                return;
            }

            // Wire the uploader once, at creation. These used to be connected
            // inside this clicked handler, so every click added another copy.
            if (!m_uploader) {
                m_uploader = new FirmwareUploader(m_model, this);

                connect(m_uploader, &FirmwareUploader::progressChanged, this,
                    [this](int pct, const QString& status) {
                        m_fwProgress->setValue(pct);
                        setShownText(m_fwStatusLabel, status);
                        m_fwStatusLabel->setAccessibleDescription(status);
                    });
                connect(m_uploader, &FirmwareUploader::finished, this,
                    [this](FirmwareUploader::Outcome outcome, const QString& msg) {
                        // Three outcomes, not two. A drained socket or a
                        // post-upload disconnect confirms nothing either way, so
                        // it must not be painted as either (#5572): only the
                        // radio's own `file update failed=` settles it. The
                        // message carries the distinction in words as well as in
                        // colour — colour alone never states it (docs/a11y.md).
                        setShownText(m_fwStatusLabel, msg);
                        m_fwStatusLabel->setAccessibleDescription(msg);
                        Tone tone = Tone::Hint;
                        switch (outcome) {
                        case FirmwareUploader::Outcome::Succeeded:
                            m_fwProgress->setValue(100);
                            tone = Tone::Ok;
                            m_fwUploadBtn->setEnabled(false);
                            break;
                        case FirmwareUploader::Outcome::Unconfirmed:
                            // Bytes left the host and the radio is probably
                            // applying them. Keep the progress bar — hiding it
                            // reads as "nothing happened" — and do not offer a
                            // retry: the uploader refuses one until reconnect.
                            m_fwProgress->setValue(100);
                            tone = Tone::Warn;
                            m_fwUploadBtn->setEnabled(false);
                            break;
                        case FirmwareUploader::Outcome::Failed:
                            m_fwProgress->hide();
                            tone = Tone::Error;
                            m_fwUploadBtn->setEnabled(true);
                            break;
                        }
                        setTone(m_fwStatusLabel, tone);
                    });
            }

            m_fwProgress->show();
            m_fwProgress->setValue(0);
            m_fwUploadBtn->setEnabled(false);
            setTone(m_fwStatusLabel, Tone::Hint);

            m_uploader->upload(m_fwFilePath);
        });

        for (auto* lbl : group->findChildren<QLabel*>()) {
            if (!hasTone(lbl))
                applyLabelStyle(lbl);
        }

        vbox->addWidget(group);
    }

    vbox->addStretch(1);
    return page;
}


QWidget* RadioSetupDialog::buildNetworkTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Model header
    {
        auto* hdr = new QHBoxLayout;
        hdr->addStretch(1);
        auto* modelLbl = new QLabel(m_model->model());
        setTone(modelLbl, Tone::Title);
        hdr->addWidget(modelLbl);
        vbox->addLayout(hdr);
    }

    // Network group
    {
        auto* group = new QGroupBox("Network");
        m_networkIdentityGroup = group;
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);

        grid->addWidget(new QLabel("IP Address:"), 0, 0);
        auto* ipLbl = makeValueLabel(displayOrDash(m_model->ip()));
        grid->addWidget(makeCopyableValueLabel(QStringLiteral("IP Address"), ipLbl), 0, 1);

        grid->addWidget(new QLabel("Subnet Mask:"), 0, 2);
        auto* maskLbl = makeValueLabel(displayOrDash(m_model->netmask()));
        grid->addWidget(makeCopyableValueLabel(QStringLiteral("Subnet Mask"), maskLbl), 0, 3);

        grid->addWidget(new QLabel("MAC Address:"), 1, 0);
        auto* macLbl = makeValueLabel(displayOrDash(m_model->mac()));
        grid->addWidget(makeCopyableValueLabel(QStringLiteral("MAC Address"), macLbl), 1, 1);

        grid->addWidget(new QLabel("Default Gateway:"), 1, 2);
        auto* gatewayLbl = makeValueLabel(displayOrDash(m_model->gateway()));
        grid->addWidget(makeCopyableValueLabel(QStringLiteral("Default Gateway"), gatewayLbl), 1, 3);

        grid->addWidget(new QLabel("Network Name:"), 2, 0);
        auto* networkNameLbl = makeValueLabel(displayOrDash(m_model->networkName()));
        // In the left pair's column, so its copy button lines up with theirs.
        grid->addWidget(makeCopyableValueLabel(QStringLiteral("Network Name"), networkNameLbl),
                        2, 1);

        connect(m_model, &RadioModel::infoChanged, this,
                [this, ipLbl, maskLbl, macLbl, gatewayLbl, networkNameLbl] {
            ipLbl->setText(displayOrDash(m_model->ip()));
            maskLbl->setText(displayOrDash(m_model->netmask()));
            macLbl->setText(displayOrDash(m_model->mac()));
            gatewayLbl->setText(displayOrDash(m_model->gateway()));
            networkNameLbl->setText(displayOrDash(m_model->networkName()));
        });

        for (auto* lbl : group->findChildren<QLabel*>())
            if (!hasTone(lbl)) applyLabelStyle(lbl);

        vbox->addWidget(group);
    }

    // Advanced group
    {
        auto* group = new QGroupBox("Advanced");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        m_privateIpPolicyLabel = new QLabel("Enforce Private IP Connections:");
        grid->addWidget(m_privateIpPolicyLabel, 0, 0);
        auto* enforceBtn = new QPushButton(m_model->enforcePrivateIp() ? "Enabled" : "Disabled");
        enforceBtn->setCheckable(true);
        enforceBtn->setChecked(m_model->enforcePrivateIp());
        connect(enforceBtn, &QPushButton::toggled, this, [this, enforceBtn](bool on) {
            enforceBtn->setText(on ? "Enabled" : "Disabled");
            m_model->sendCommand(
                QString("radio set enforce_private_ip_connections=%1").arg(on ? 1 : 0));
        });
        m_privateIpPolicyControl = enforceBtn;
        grid->addWidget(enforceBtn, 0, 1);

        // 128-bit hex token generator — plenty for a local same-user secret.
        // global() is only a securely *seeded* general-purpose PRNG (Xoshiro),
        // whose stream is recoverable from enough output; system() is the OS
        // CSPRNG Qt documents for keys and secrets (#4181).
        auto genToken = []() -> QString {
            quint64 a = QRandomGenerator::system()->generate64();
            quint64 b = QRandomGenerator::system()->generate64();
            return QStringLiteral("%1%2")
                .arg(a, 16, 16, QLatin1Char('0'))
                .arg(b, 16, 16, QLatin1Char('0'));
        };
        // The token field is created here (before the toggle) so the toggle's
        // enable handler can auto-fill it — enabling the bridge without a
        // token would leave it open, which defeats the point. The token lives
        // in the OS secret store and reads ASYNCHRONOUSLY; tokenLoaded gates
        // the auto-mint so a toggle fired before the read lands can't clobber
        // an existing token.
        auto* tokenEdit = new QLineEdit;
        tokenEdit->setReadOnly(true);
        tokenEdit->setPlaceholderText("(loading…)");
        tokenEdit->setToolTip(
            "Export this as AETHER_MCP_TOKEN only in the shell session that\n"
            "launches your AI assistant, without recording it in shell history.\n"
            "Never put it in an MCP config file or a shell profile.\n"
            "Only a client holding this token can drive the radio.\n"
            "Stored in your OS secret store. See\n"
            + DocsLinks::automationBridge());
        auto tokenLoaded = std::make_shared<bool>(false);
        AutomationBridgeSettings::loadToken(this,
            [tokenEdit, tokenLoaded](const QString& tok) {
                tokenEdit->setText(tok);
                tokenEdit->setPlaceholderText(
                    "(none — enable the bridge or click Rotate)");
                *tokenLoaded = true;
            });

        // Agent automation bridge (#3646) — the endpoint MCP clients (AI
        // coding assistants) connect to for validating changes against the
        // running app. Off by default; the operator opts in here. The
        // AETHER_AUTOMATION launch env var still force-enables it regardless
        // of this toggle (headless/CI path), so we reflect either source.
        {
            grid->addWidget(new QLabel("Agent Automation (MCP):"), 1, 0);
            const bool bridgeOn = AutomationBridgeSettings::enabled()
                || AutomationBridgeSettings::envForced();
            auto* mcpBtn = new QPushButton(bridgeOn ? "Enabled" : "Disabled");
            mcpBtn->setCheckable(true);
            mcpBtn->setChecked(bridgeOn);
            mcpBtn->setToolTip(
                "Enable the in-app automation bridge so an AI coding assistant "
                "(via the MCP server, tools/aether_mcp.py) can introspect and\n"
                "drive this app to validate changes. Off by default. Transmit-"
                "keying controls stay blocked unless the app is launched with\n"
                "AETHER_AUTOMATION_ALLOW_TX. See\n"
                + DocsLinks::automationBridge());
            // Env-var force-enable wins and can't be turned off from the UI —
            // make that visible rather than letting a toggle silently no-op.
            if (AutomationBridgeSettings::envForced()) {
                mcpBtn->setEnabled(false);
                mcpBtn->setToolTip(mcpBtn->toolTip()
                    + "\n\nForced on by the AETHER_AUTOMATION launch environment variable.");
            }
            m_automationBridgeBtn = mcpBtn;
            connect(mcpBtn, &QPushButton::toggled, this,
                    [this, mcpBtn, tokenEdit, genToken, tokenLoaded](bool on) {
                mcpBtn->setText(on ? "Enabled" : "Disabled");
                // Persist the click as the operator's INTENT right away, so a
                // quit or a reopened dialog during the async token read still
                // sees it. Enabling is not the same as listening, though: the
                // socket only binds once the read lands, so
                // MainWindow::startAutomationBridge() records the real outcome
                // (AutomationBridgeSettings::recordStartOutcome) even if this
                // dialog has closed — a failed bind clears the flag again
                // rather than leaving enabled=true with nothing listening and
                // every launch silently re-attempting the doomed start (#4181).
                AutomationBridgeSettings::setEnabled(on);
                // Enabling with no token yet → mint one so the bridge is never
                // exposed without auth. Only when the async token read has
                // landed (tokenLoaded) so we can't clobber an existing token.
                if (on && *tokenLoaded && tokenEdit->text().isEmpty()) {
                    const QString tok = genToken();
                    tokenEdit->setText(tok);
                    AutomationBridgeSettings::saveToken(tok);
                    QGuiApplication::clipboard()->setText(tok);
                    emit automationBridgeTokenRotated(tok);
                }
                emit automationBridgeToggled(on);
            });
            grid->addWidget(mcpBtn, 1, 1);
        }

        // Access token row — display the shared secret with Copy + Rotate.
        // Rotate makes a new token and applies it immediately, locking out any
        // client still using the old one.
        {
            grid->addWidget(new QLabel("Access Token:"), 2, 0);
            auto* tokenRow = new QWidget;
            auto* tokLay = new QHBoxLayout(tokenRow);
            tokLay->setContentsMargins(0, 0, 0, 0);
            tokLay->setSpacing(6);

            auto* copyBtn = new QPushButton("Copy");
            auto* rotateBtn = new QPushButton("Rotate");
            copyBtn->setToolTip("Copy the token to the clipboard.");
            rotateBtn->setToolTip(
                "Generate a new token and apply it immediately. Any client still\n"
                "using the old token is locked out until you update its config.");

            connect(copyBtn, &QPushButton::clicked, this, [tokenEdit]() {
                if (!tokenEdit->text().isEmpty())
                    QGuiApplication::clipboard()->setText(tokenEdit->text());
            });
            connect(rotateBtn, &QPushButton::clicked, this,
                    [this, tokenEdit, genToken]() {
                const QString tok = genToken();
                tokenEdit->setText(tok);
                AutomationBridgeSettings::saveToken(tok);
                QGuiApplication::clipboard()->setText(tok);
                emit automationBridgeTokenRotated(tok);
            });

            tokLay->addWidget(tokenEdit, 1);
            tokLay->addWidget(copyBtn);
            tokLay->addWidget(rotateBtn);
            grid->addWidget(tokenRow, 2, 1);
        }

        // Allow TX via MCP — the transmit-keying guard. Off by default; the
        // bridge refuses MOX/PTT/TUNE/ATU/CWX unless this (or the
        // AETHER_AUTOMATION_ALLOW_TX env var) is set. Checking the box the
        // first time raises a confirmation with the operator-responsibility
        // warning; once confirmed the choice persists and is not re-prompted.
        {
            grid->addWidget(new QLabel("Allow TX via MCP:"), 3, 0);
            auto* txCheck = new QCheckBox("Enable transmit control");
            const bool envForcesTx = qEnvironmentVariableIsSet("AETHER_AUTOMATION_ALLOW_TX");
            const bool envBlocksTx = qEnvironmentVariableIsSet("AETHER_AUTOMATION_NO_TX");
            txCheck->setChecked(!envBlocksTx
                                && (AutomationBridgeSettings::txAllowed() || envForcesTx));
            txCheck->setToolTip(
                "Let an MCP client key the transmitter (MOX/PTT/TUNE/ATU/CWX).\n"
                "OFF by default — the bridge blocks all transmit-keying otherwise.\n"
                "Bridge-originated TX is limited by a force-unkey watchdog. You are\n"
                "responsible for anything transmitted. See\n"
                + DocsLinks::automationBridge());
            if (envBlocksTx) {
                txCheck->setEnabled(false);
                txCheck->setToolTip(txCheck->toolTip()
                    + "\n\nPinned off by the AETHER_AUTOMATION_NO_TX launch variable.");
            } else if (envForcesTx) {
                txCheck->setEnabled(false);
                txCheck->setToolTip(txCheck->toolTip()
                    + "\n\nForced on by the AETHER_AUTOMATION_ALLOW_TX launch variable.");
            }
            connect(txCheck, &QCheckBox::toggled, this, [this, txCheck](bool on) {
                const QPointer<RadioSetupDialog> self(this);
                const QPointer<QCheckBox> txCheckGuard(txCheck);
                if (on && !AutomationBridgeSettings::txAck()) {
                    // First-time enable → confirm. Operator must acknowledge.
                    ScopedChildWidget<QMessageBox> boxOwner(this);
                    QMessageBox& box = *boxOwner.get();
                    box.setIcon(QMessageBox::Warning);
                    box.setWindowTitle("Allow TX via MCP?");
                    box.setText("Allow an AI assistant / MCP client to key the transmitter?");
                    box.setInformativeText(
                        "This lets any MCP client holding the access token drive "
                        "transmit-keying controls — MOX/PTT, TUNE, the ATU, and CWX "
                        "send — on your radio. Automated software will be able to put "
                        "a signal on the air.\n\n"
                        "A force-unkey watchdog limits bridge-originated TX, but it is "
                        "a backstop, not a guarantee. You, the operator, are "
                        "ultimately responsible for all transmissions from your station "
                        "— including their content, timing, frequency, power, and "
                        "compliance with your license and local regulations.\n\n"
                        "Only enable this if you understand and accept that "
                        "responsibility. You can turn it off again at any time.");
                    auto* confirm = box.addButton("Confirm — allow TX", QMessageBox::AcceptRole);
                    box.addButton("Cancel", QMessageBox::RejectRole);
                    box.setDefaultButton(qobject_cast<QPushButton*>(box.buttons().value(1)));
                    box.exec();
                    if (!self || !txCheckGuard || !boxOwner) {
                        return;
                    }
                    if (box.clickedButton() != confirm) {
                        // Cancelled — revert without persisting or emitting.
                        QSignalBlocker blocker(txCheckGuard.data());
                        txCheckGuard->setChecked(false);
                        return;
                    }
                    // Confirmed — remember the acknowledgement so we never
                    // prompt again on a future enable.
                    AutomationBridgeSettings::setTxAck(true);
                }
                AutomationBridgeSettings::setTxAllowed(on);
                emit automationBridgeTxAllowedChanged(on);
            });
            grid->addWidget(txCheck, 3, 1);
        }

        // Observe only — the read-only gate (#4188 area 6). When checked, the
        // bridge refuses every mutating verb (set/invoke/connect/tune/capture…)
        // and answers only pure-introspection reads. Enforced in the bridge, so
        // no MCP client can flip it off. Lets the operator start the app, arm
        // observe-only, then start the MCP server for a look-but-don't-touch
        // session. An env override (AETHER_AUTOMATION_READONLY) pins it for
        // headless/CI runs.
        {
            grid->addWidget(new QLabel("Observe only:"), 4, 0);
            auto* roCheck = new QCheckBox("Read-only (block all driving)");
            roCheck->setObjectName(QStringLiteral("automationReadOnlyCheck"));
            const bool envForcesRo = qEnvironmentVariableIsSet("AETHER_AUTOMATION_READONLY");
            roCheck->setChecked(AutomationBridgeSettings::readOnly() || envForcesRo);
            roCheck->setToolTip(
                "Make the bridge observe-only: MCP clients can read state\n"
                "(ping/whoami/get/dumpTree/grab, read-only log and streams\n"
                "actions, floors, and hitTest)\n"
                "but every mutating verb is refused. Enforced in the app, so a\n"
                "client cannot bypass it. Toggle takes effect immediately on the\n"
                "running bridge. See\n"
                + DocsLinks::automationBridge());
            if (envForcesRo) {
                roCheck->setEnabled(false);
                roCheck->setToolTip(roCheck->toolTip()
                    + "\n\nForced on by the AETHER_AUTOMATION_READONLY launch variable.");
            }
            connect(roCheck, &QCheckBox::toggled, this, [this](bool on) {
                AutomationBridgeSettings::setReadOnly(on);
                emit automationBridgeReadOnlyChanged(on);
            });
            grid->addWidget(roCheck, 4, 1);
        }

        m_networkMtuLabel = new QLabel("Network MTU:");
        grid->addWidget(m_networkMtuLabel, 5, 0);
        auto* mtuSpin = new QSpinBox;
        mtuSpin->setRange(kMinNetworkMtu, kMaxNetworkMtu);
        mtuSpin->setValue(m_model->networkMtuSetting());
        mtuSpin->setSuffix(" bytes");
        mtuSpin->setToolTip(QString("Maximum Transmission Unit for VITA-49 UDP packets.\n"
                                    "Default: %1 (compatible with most VPN/SD-WAN tunnels).\n"
                                    "Over Tailscale the radio is sent at most %2.")
                                .arg(kDefaultNetworkMtu)
                                .arg(kTailnetNetworkMtu));
        connect(mtuSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int val) {
            AppSettings::instance().setValue("NetworkMtu", QString::number(val));
            AppSettings::instance().save();
            // Over Tailscale the radio gets the capped value (#5949), never
            // more than the tunnel carries.
            m_model->sendCommand(QString("client set enforce_network_mtu=1 network_mtu=%1")
                                     .arg(m_model->networkMtuParam(val)));
        });
        m_networkMtuControl = mtuSpin;
        // A four-digit value: its own width, not the token field's.
        mtuSpin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        grid->addWidget(mtuSpin, 5, 1, Qt::AlignLeft);

        // VITA-49 UDP receive buffer (SO_RCVBUF). Snap-to-preset slider; the
        // kernel clamps the grant at net.core.rmem_max, so we show the granted
        // size live (PanadapterStream::receiveBufferApplied) — the slider never
        // claims more than the system actually gives. (#3810)
        static const int kRcvBufPresets[] = {
            256 * 1024, 512 * 1024, 1024 * 1024, 2 * 1024 * 1024, 4 * 1024 * 1024
        };
        constexpr int kRcvBufPresetCount = 5;
        auto fmtBytes = [](int b) -> QString {
            if (b >= 1024 * 1024)
                return QStringLiteral("%1 MB").arg(
                    QString::number(b / (1024.0 * 1024.0), 'g', 3));
            return QStringLiteral("%1 KB").arg(b / 1024);
        };

        m_vitaReceiveBufferLabel = new QLabel("VITA-49 RX buffer:");
        grid->addWidget(m_vitaReceiveBufferLabel, 6, 0);
        auto* bufRow = new QWidget;
        auto* bufLay = new QHBoxLayout(bufRow);
        bufLay->setContentsMargins(0, 0, 0, 0);
        bufLay->setSpacing(8);
        auto* bufSlider = new QSlider(Qt::Horizontal);
        bufSlider->setRange(0, kRcvBufPresetCount - 1);
        bufSlider->setSingleStep(1);
        bufSlider->setPageStep(1);
        bufSlider->setTickPosition(QSlider::TicksBelow);
        bufSlider->setTickInterval(1);
        bufSlider->setToolTip(
            "Kernel receive buffer for the VITA-49 stream socket.\n"
            "Larger absorbs panadapter/waterfall bursts so they aren't dropped\n"
            "(dropped packets look like network-stat dips). Default 4 MB.\n"
            "The system caps this at net.core.rmem_max — see the granted size.");
        // Initial position: smallest preset >= the persisted request.
        {
            const int cur = AetherSDR::NetworkSettings::vitaReceiveBufferBytes();
            int idx = kRcvBufPresetCount - 1;
            for (int i = 0; i < kRcvBufPresetCount; ++i)
                if (kRcvBufPresets[i] >= cur) { idx = i; break; }
            bufSlider->setValue(idx);
        }
        auto* bufValLabel = new QLabel(fmtBytes(kRcvBufPresets[bufSlider->value()]));
        bufValLabel->setMinimumWidth(48);
        bufLay->addWidget(bufSlider, 1);
        bufLay->addWidget(bufValLabel);
        m_vitaReceiveBufferControls = bufRow;
        grid->addWidget(bufRow, 6, 1);

        auto* bufGrantedLabel = new QLabel;
        if (m_model && m_model->panStream()) {
            const int g = m_model->panStream()->grantedReceiveBufferBytes();
            bufGrantedLabel->setText(g > 0 ? QString("granted: %1").arg(fmtBytes(g))
                                           : QStringLiteral("granted: — (applies on connect)"));
        }
        m_vitaReceiveBufferStatus = bufGrantedLabel;
        grid->addWidget(bufGrantedLabel, 7, 1);

        connect(bufSlider, &QSlider::valueChanged, this,
                [this, bufValLabel, fmtBytes](int idx) {
            const int bytes = kRcvBufPresets[std::clamp(idx, 0, kRcvBufPresetCount - 1)];
            bufValLabel->setText(fmtBytes(bytes));
            AetherSDR::NetworkSettings::setVitaReceiveBufferBytes(bytes);
            // Re-apply live on the network worker thread (the socket lives there).
            if (m_model && m_model->panStream()) {
                QMetaObject::invokeMethod(
                    m_model->panStream(),
                    [stream = m_model->panStream(), bytes]() {
                        stream->setReceiveBufferSizeBytes(bytes);
                    });
            }
        });
        // Live granted-size feedback (cross-thread → queued).
        if (m_model && m_model->panStream()) {
            connect(m_model->panStream(), &PanadapterStream::receiveBufferApplied, this,
                    [bufGrantedLabel, fmtBytes](int requested, int granted) {
                QString t = QString("granted: %1").arg(fmtBytes(granted));
                if (granted < requested)
                    t += QStringLiteral("  (capped by net.core.rmem_max)");
                bufGrantedLabel->setText(t);
            });
        }

        for (auto* lbl : group->findChildren<QLabel*>())
            if (!hasTone(lbl)) applyLabelStyle(lbl);

        updateRadioCapabilityVisibility();

        vbox->addWidget(group);
    }

    // DHCP / Static IP group
    vbox->addWidget(buildIpConfigGroup());

    vbox->addStretch(1);
    return page;
}

// Extracted to reduce lambda nesting depth in buildNetworkTab()
// (avoids GCC 13 internal compiler error on Ubuntu 24.04)
QGroupBox* RadioSetupDialog::buildIpConfigGroup()
{
    auto* group = new QGroupBox("IP Configuration");
    auto* gvbox = new QVBoxLayout(group);
    gvbox->setSpacing(6);

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(4);

    const bool isStatic = m_model->hasStaticIp();
    const bool canConfigure = m_model->backendCapabilities().hasClientNetworkConfig;
    const QString unavailableTip = tr("Changing the radio's IP configuration is not supported by this radio.");

    auto* dhcpBtn = new QPushButton("DHCP");
    m_ipDhcpButton = dhcpBtn;
    dhcpBtn->setCheckable(true);
    dhcpBtn->setChecked(!isStatic);
    dhcpBtn->setEnabled(canConfigure);
    dhcpBtn->setToolTip(canConfigure ? QString() : unavailableTip);
    setButtonRole(dhcpBtn, ButtonRole::Select);
    btnRow->addWidget(dhcpBtn);

    auto* staticBtn = new QPushButton("Static");
    m_ipStaticButton = staticBtn;
    staticBtn->setCheckable(true);
    staticBtn->setChecked(isStatic);
    staticBtn->setEnabled(canConfigure);
    staticBtn->setToolTip(dhcpBtn->toolTip());
    setButtonRole(staticBtn, ButtonRole::Select);
    btnRow->addWidget(staticBtn);

    btnRow->addStretch(1);

    // Mode, then the three addresses, captioned in one column like every
    // other group; an address field is sized for 255.255.255.255.
    auto* fieldsGrid = new QGridLayout;
    fieldsGrid->setHorizontalSpacing(8);
    fieldsGrid->setVerticalSpacing(6);
    fieldsGrid->addWidget(new QLabel("Mode:"), 0, 0);
    fieldsGrid->addLayout(btnRow, 0, 1);

    fieldsGrid->addWidget(new QLabel("IP Address:"), 1, 0);
    auto* staticIp = new QLineEdit(isStatic ? m_model->staticIp() : m_model->ip());
    m_staticIpEdit = staticIp;
    staticIp->setEnabled(canConfigure && isStatic);
    fieldsGrid->addWidget(staticIp, 1, 1);

    fieldsGrid->addWidget(new QLabel("Mask:"), 2, 0);
    auto* staticMask = new QLineEdit(isStatic ? m_model->staticNetmask() : m_model->netmask());
    m_staticMaskEdit = staticMask;
    staticMask->setEnabled(canConfigure && isStatic);
    fieldsGrid->addWidget(staticMask, 2, 1);

    fieldsGrid->addWidget(new QLabel("Gateway:"), 3, 0);
    auto* staticGw = new QLineEdit(isStatic ? m_model->staticGateway() : m_model->gateway());
    m_staticGatewayEdit = staticGw;
    staticGw->setEnabled(canConfigure && isStatic);
    fieldsGrid->addWidget(staticGw, 3, 1);
    for (QLineEdit* edit : {staticIp, staticMask, staticGw})
        edit->setFixedWidth(180);

    gvbox->addLayout(fieldsGrid);
    for (auto* lbl : group->findChildren<QLabel*>())
        if (!hasTone(lbl)) applyLabelStyle(lbl);

    auto* applyBtn = new QPushButton("Apply");
    m_ipApplyButton = applyBtn;
    m_ipConfigPresentation.sessionKey = m_model->isConnected()
        ? QStringLiteral("%1:%2").arg(m_model->family(), m_model->serial())
        : QString();
    m_ipConfigPresentation.canConfigure = canConfigure;
    applyBtn->setEnabled(false);
    applyBtn->setToolTip(canConfigure ? QString() : unavailableTip);
    applyBtn->setAccessibleDescription(applyBtn->toolTip());
    fieldsGrid->addWidget(applyBtn, 4, 1, Qt::AlignLeft);

    connect(dhcpBtn, &QPushButton::clicked, this,
            [dhcpBtn, staticBtn, staticIp, staticMask, staticGw, applyBtn] {
        dhcpBtn->setChecked(true);
        staticBtn->setChecked(false);
        staticIp->setEnabled(false);
        staticMask->setEnabled(false);
        staticGw->setEnabled(false);
        applyBtn->setEnabled(true);
    });
    connect(staticBtn, &QPushButton::clicked, this,
            [dhcpBtn, staticBtn, staticIp, staticMask, staticGw, applyBtn] {
        dhcpBtn->setChecked(false);
        staticBtn->setChecked(true);
        staticIp->setEnabled(true);
        staticMask->setEnabled(true);
        staticGw->setEnabled(true);
        applyBtn->setEnabled(true);
    });
    connect(applyBtn, &QPushButton::clicked, this,
            [this, dhcpBtn, staticIp, staticMask, staticGw, applyBtn] {
        if (dhcpBtn->isChecked()) {
            m_model->sendCommand("radio static_net_params reset");
            qDebug() << "RadioSetupDialog: network set to DHCP";
        } else {
            const QString cmd = QString("radio static_net_params ip=%1 gateway=%2 netmask=%3")
                .arg(staticIp->text(), staticGw->text(), staticMask->text());
            m_model->sendCommand(cmd);
            qDebug() << "RadioSetupDialog: static IP applied" << cmd;
        }
        applyBtn->setEnabled(false);
    });

    return group;
}
QWidget* RadioSetupDialog::buildGpsTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Model header
    {
        auto* hdr = new QHBoxLayout;
        hdr->addStretch(1);
        auto* modelLbl = new QLabel(m_model->model());
        setTone(modelLbl, Tone::Title);
        hdr->addWidget(modelLbl);
        vbox->addLayout(hdr);
    }

    // GPS installed status
    {
        const bool installed = m_model->hasGpsSetupHardware();
        auto* statusLbl = new QLabel(installed ? "GPS is installed" : "GPS is not installed");
        setTone(statusLbl, installed ? Tone::Ok : Tone::Error);
        statusLbl->setAlignment(Qt::AlignCenter);
        vbox->addWidget(statusLbl);
        vbox->addSpacing(16);
    }

    // GPS data grid
    {
        auto* grid = new QGridLayout;
        grid->setSpacing(8);

        auto addField = [&](int row, int col, const QString& label, const QString& value) {
            auto* lbl = new QLabel(label);
            applyLabelStyle(lbl);
            grid->addWidget(lbl, row, col * 2);
            auto* val = makeValueLabel(value);
            grid->addWidget(val, row, col * 2 + 1);
        };

        addField(0, 0, "Latitude:",     m_model->gpsLat());
        addField(0, 1, "Longitude:",    m_model->gpsLon());
        addField(1, 0, "Grid Square:",  m_model->gpsGrid());
        addField(1, 1, "Altitude:",     m_model->gpsAltitude());
        addField(2, 0, "Sat Tracked:",  QString::number(m_model->gpsTracked()));
        addField(2, 1, "Sat Visible:",  QString::number(m_model->gpsVisible()));
        addField(3, 0, "Speed:",        m_model->gpsSpeed());
        addField(3, 1, "Freq Error:",   m_model->gpsFreqError());
        addField(4, 0, "Status:",       m_model->gpsStatus());
        addField(4, 1, "UTC Time:",     m_model->gpsTime());

        vbox->addLayout(grid);
    }

    vbox->addStretch(1);
    return page;
}
QWidget* RadioSetupDialog::buildTxTab()
{
    auto& tx = m_model->transmitModel();
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Model header
    {
        auto* hdr = new QHBoxLayout;
        hdr->addStretch(1);
        auto* modelLbl = new QLabel(m_model->model());
        setTone(modelLbl, Tone::Title);
        hdr->addWidget(modelLbl);
        vbox->addLayout(hdr);
    }

    // The TX profile and the band settings are not timings: built with the
    // Timings group as before, but laid out in Options below.
    QComboBox* profCmb = nullptr;
    QPushButton* bandSetBtn = nullptr;

    // Timings group
    {
        auto* group = new QGroupBox("Timings (in ms)");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        QList<QLineEdit*> timingEdits;
        auto addTimingField = [&](int row, int col, const QString& label, int value) {
            auto* lbl = new QLabel(label);
            applyLabelStyle(lbl);
            grid->addWidget(lbl, row, col * 2);
            auto* edit = new QLineEdit(QString::number(value));
            edit->setFixedWidth(60);
            edit->setAccessibleName(QString(label).remove(QLatin1Char(':')));
            grid->addWidget(edit, row, col * 2 + 1);
            timingEdits.append(edit);
            return edit;
        };

        // `interlock set` is Flex wire text, and nothing else reads these
        // fields: a connected radio with no command plane takes no timing from
        // here. By hand, not ControlAvailabilityRegistry: command-plane
        // reachability is not in RadioCapabilities.
        const auto timingFieldsAvailable = [this] {
            return !m_model->isConnected() || m_model->hasCommandPlane();
        };

        // Scale factor lets the same helper drive both 1:1 ms fields and
        // the seconds-displayed timeout field which the radio still
        // expects in ms (FlexLib Radio.cs:7463 — "in milliseconds").
        auto connectTimingField = [&](QLineEdit* edit, const QString& key, int scale = 1) {
            connect(edit, &QLineEdit::editingFinished, this,
                    [this, edit, key, scale, timingFieldsAvailable] {
                if (!timingFieldsAvailable()) {
                    return;
                }
                int val = qMax(0, edit->text().toInt());
                edit->setText(QString::number(val));
                m_model->sendCommand(QString("interlock set %1=%2").arg(key).arg(val * scale));
            });
        };

        auto* accTxEdit   = addTimingField(0, 0, "ACC TX:",       tx.accTxDelay());
        auto* txDelayEdit = addTimingField(0, 1, "TX Delay:",      tx.txDelay());
        auto* tx1Edit     = addTimingField(1, 0, "RCA TX1:",       tx.tx1Delay());
        // Timeout stored on the radio in milliseconds (FlexLib Radio.cs:7463);
        // display in whole seconds for readability — minutes lose too much
        // resolution for short-cycle TOT settings.
        auto* timeoutEdit = addTimingField(1, 1, "Timeout (sec):", tx.interlockTimeout() / 1000);
        auto* tx2Edit     = addTimingField(2, 0, "RCA TX2:",       tx.tx2Delay());

        connectTimingField(accTxEdit,   "acc_tx_delay");
        connectTimingField(txDelayEdit, "tx_delay");
        connectTimingField(tx1Edit,     "tx1_delay");
        connectTimingField(timeoutEdit, "timeout", 1000);
        connectTimingField(tx2Edit,     "tx2_delay");

        // TX Profile dropdown (below Timeout, right column)
        profCmb = new QComboBox;
        profCmb->addItems(tx.profileList());
        profCmb->setCurrentText(tx.activeProfile());
        AetherSDR::applyComboStyle(profCmb);
        connect(profCmb, &QComboBox::currentTextChanged, this, [this](const QString& name) {
            m_model->transmitModel().loadProfile(name);
        });

        auto* tx3Edit = addTimingField(3, 0, "RCA TX3:", tx.tx3Delay());
        connectTimingField(tx3Edit, "tx3_delay");

        // TX Band Settings button
        bandSetBtn = new QPushButton("TX Band Settings");
        connect(bandSetBtn, &QPushButton::clicked, this, [this] {
            emit txBandSettingsRequested();
        });

        for (auto* lbl : group->findChildren<QLabel*>())
            if (!hasTone(lbl)) applyLabelStyle(lbl);

        // Last in the group: the lambda copies timingEdits, so every field
        // has to exist before this point.
        const auto applyTimingAvailability = [timingEdits, timingFieldsAvailable]() {
            const bool available = timingFieldsAvailable();
            const QString why = available
                ? QString()
                : tr("Unavailable: this radio does not accept transmit timings "
                     "from here. No delay or timeout set here is applied.");
            for (QLineEdit* edit : timingEdits) {
                edit->setEnabled(available);
                edit->setToolTip(why);
                edit->setAccessibleDescription(why);
            }
        };
        applyTimingAvailability();
        connect(m_model, &RadioModel::connectionStateChanged, group,
                applyTimingAvailability);

        vbox->addWidget(group);
    }

    // Interlocks group
    {
        auto* group = new QGroupBox("Interlocks - TX REQ");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        auto* rcaLbl = new QLabel("RCA:");
        applyLabelStyle(rcaLbl);
        grid->addWidget(rcaLbl, 0, 0);
        auto* rcaCmb = new QComboBox;
        rcaCmb->addItems({"Active Low", "Active High"});
        rcaCmb->setCurrentIndex(tx.rcaTxReqPolarity());
        AetherSDR::applyComboStyle(rcaCmb);
        grid->addWidget(rcaCmb, 0, 1);

        auto* accLbl = new QLabel("Accessory:");
        applyLabelStyle(accLbl);
        grid->addWidget(accLbl, 0, 2);
        auto* accCmb = new QComboBox;
        accCmb->addItems({"Active Low", "Active High"});
        accCmb->setCurrentIndex(tx.accTxReqPolarity());
        AetherSDR::applyComboStyle(accCmb);
        grid->addWidget(accCmb, 0, 3);

        // PTT Mic Override (SmartSDR v4.3, SMART-9340): from 4.3 a hardware
        // PTT transmits the selected TX source, DAX and remote audio included;
        // on, it forces the analog mic as firmware before 4.3 always did.
        // Radio-wide, and only a 4.3 radio reports it (#6442).
        auto* pttOvrLbl = new QLabel("PTT Mic Override:");
        applyLabelStyle(pttOvrLbl);
        grid->addWidget(pttOvrLbl, 1, 0);
        auto* pttOvrBtn = new QPushButton;
        pttOvrBtn->setCheckable(true);
        setButtonRole(pttOvrBtn, ButtonRole::Select);
        pttOvrBtn->setAccessibleName(tr("PTT Mic Override"));
        grid->addWidget(pttOvrBtn, 1, 1, Qt::AlignLeft);
        const auto syncPttOverride = [this, pttOvrBtn]() {
            const auto& t = m_model->transmitModel();
            const bool reported = t.havePttMicOverride();
            QSignalBlocker block(pttOvrBtn);
            pttOvrBtn->setChecked(t.pttMicOverride());
            pttOvrBtn->setText(t.pttMicOverride() ? "Enabled" : "Disabled");
            pttOvrBtn->setEnabled(reported);
            const QString tip = reported
                ? tr("Enabled: a hardware PTT (RCA PTT jack, ACC or mic-jack PTT) "
                     "transmits the analog mic, even when DAX or PC audio is the TX source.\n"
                     "Disabled (firmware 4.3 default): a hardware PTT transmits the "
                     "selected TX source, as MOX does.\n"
                     "This setting is shared by every client on the radio.")
                : tr("Requires SmartSDR v4.3 firmware or later. Earlier firmware "
                     "always transmits the analog mic on a hardware PTT.");
            pttOvrBtn->setToolTip(tip);
            pttOvrBtn->setAccessibleDescription(tip);
        };
        syncPttOverride();
        // The button shows the radio's value: a click only requests the change
        // and snaps back, and the transmit-status echo moves it.
        connect(pttOvrBtn, &QPushButton::toggled, this, [this, syncPttOverride](bool on) {
            m_model->transmitModel().setPttMicOverride(on);
            syncPttOverride();
        });
        connect(&tx, &TransmitModel::stateChanged, pttOvrBtn, syncPttOverride);
        // resetState() on disconnect announces through micStateChanged, not
        // stateChanged; without this the page keeps the last session's value.
        connect(&tx, &TransmitModel::micStateChanged, pttOvrBtn, syncPttOverride);
        // A family switch clears the report without either signal.
        connect(m_model, &RadioModel::backendRebuilt, pttOvrBtn, syncPttOverride);

        vbox->addWidget(group);
    }

    // Max Power / Show TX in Waterfall / Slice-TX Follow. Tune Mode is not a
    // setting: it is a one-shot from the TUNE button's right-click menu in
    // TxApplet and is never written to AppSettings.
    {
        auto* grid = new QGridLayout;
        grid->setSpacing(6);

        auto* mpLbl = new QLabel("Max Power:");
        applyLabelStyle(mpLbl);
        grid->addWidget(mpLbl, 0, 0);
        // A backend declaring txPowerBands reports maxPowerLevel() as the
        // band's rated watts, shown only once haveMaxPowerLevel() says it was
        // reported (#5637). Without a command plane the field is read-only,
        // since its write would be dropped. With one, it is the original Flex
        // field: same unit, editor, clamp and write.
        const bool maxPowerIsRatedWatts =
            !m_model->backendCapabilities().txPowerBands.isEmpty();
        const bool ratedWattsReported = maxPowerIsRatedWatts
            && tx.haveMaxPowerLevel() && tx.maxPowerLevel() > 0;
        const bool maxPowerWritable =
            !maxPowerIsRatedWatts && m_model->hasCommandPlane();

        auto* mpRow = new QHBoxLayout;
        auto* mpEdit = new QLineEdit(
            maxPowerIsRatedWatts && !ratedWattsReported
                ? QString()
                : QString::number(tx.maxPowerLevel()));
        mpEdit->setFixedWidth(50);
        mpEdit->setAccessibleName(tr("Max Power"));
        mpRow->addWidget(mpEdit);
        auto* mpUnit = new QLabel(
            !maxPowerIsRatedWatts ? QStringLiteral("%")
            : ratedWattsReported  ? QStringLiteral("W")
                                  : QString());
        applyLabelStyle(mpUnit);
        mpRow->addWidget(mpUnit);
        mpRow->addStretch(1);
        grid->addLayout(mpRow, 0, 1);

        if (maxPowerWritable) {
            connect(mpEdit, &QLineEdit::editingFinished, this, [this, mpEdit] {
                int val = qBound(0, mpEdit->text().toInt(), 100);
                mpEdit->setText(QString::number(val));
                m_model->sendCommand(
                    QString("transmit set max_power_level=%1").arg(val));
            });
        } else {
            // Read-only, not disabled, so it stays focusable; the reason goes
            // on accessibleDescription too (docs/a11y.md).
            const QString why = ratedWattsReported
                ? tr("The radio's rated output. This radio does not accept a "
                     "maximum power setting from here.")
                : maxPowerIsRatedWatts
                ? tr("The radio has not reported its rated output yet. This "
                     "radio does not accept a maximum power setting from "
                     "here.")
                : tr("This radio does not accept a maximum power setting "
                     "from here.");
            mpEdit->setReadOnly(true);
            mpEdit->setToolTip(why);
            mpEdit->setAccessibleDescription(why);
        }

        auto* swLbl = new QLabel("Show TX in Waterfall:");
        applyLabelStyle(swLbl);
        grid->addWidget(swLbl, 1, 0);
        auto* swBtn = new QPushButton(tx.showTxInWaterfall() ? "Enabled" : "Disabled");
        swBtn->setCheckable(true);
        swBtn->setChecked(tx.showTxInWaterfall());
        connect(swBtn, &QPushButton::toggled, this, [this, swBtn](bool on) {
            swBtn->setText(on ? "Enabled" : "Disabled");
            // Where the backend declares the client owns this flag, the
            // model takes it itself. Everywhere else that returns false and
            // the wire text is sent, which only a Flex answers.
            if (!m_model->requestLocalShowTxInWaterfall(on)) {
                m_model->sendCommand(
                    QString("transmit set show_tx_in_waterfall=%1").arg(on ? 1 : 0));
            }
        });
        grid->addWidget(swBtn, 1, 1);

        // Slice–TX Follow Mode (#441, #1351) — mutually exclusive toggles
        auto* followLbl = new QLabel("Slice/TX Follow:");
        applyLabelStyle(followLbl);
        grid->addWidget(followLbl, 2, 0);


        bool txFollows = AppSettings::instance().value("TxFollowsActiveSlice", "False").toString() == "True";
        auto* tfBtn = new QPushButton("TX Follows Active Slice");
        tfBtn->setCheckable(true);
        tfBtn->setChecked(txFollows);
        tfBtn->setToolTip("TX follows the active slice.\nDisabled during Split operation.");

        bool activeFollows = AppSettings::instance().value("ActiveFollowsTxSlice", "False").toString() == "True";
        auto* afBtn = new QPushButton("Active Slice Follows TX");
        afBtn->setCheckable(true);
        afBtn->setChecked(activeFollows);
        afBtn->setToolTip("Switch active slice when TX moves externally\n(e.g. WSJT-X or CAT command).");

        auto* followRow = new QHBoxLayout;
        followRow->setSpacing(6);
        followRow->addWidget(tfBtn);
        followRow->addWidget(afBtn);
        followRow->addStretch(1);
        grid->addLayout(followRow, 2, 1);

        // Mutual exclusion: enabling one disables the other
        connect(tfBtn, &QPushButton::toggled, this, [tfBtn, afBtn](bool on) {
            Q_UNUSED(tfBtn);
            auto& s = AppSettings::instance();
            s.setValue("TxFollowsActiveSlice", on ? "True" : "False");
            if (on && afBtn->isChecked()) {
                afBtn->setChecked(false);
            }
            s.save();
        });
        connect(afBtn, &QPushButton::toggled, this, [tfBtn, afBtn](bool on) {
            Q_UNUSED(afBtn);
            auto& s = AppSettings::instance();
            s.setValue("ActiveFollowsTxSlice", on ? "True" : "False");
            if (on && tfBtn->isChecked()) {
                tfBtn->setChecked(false);
            }
            s.save();
        });

        auto* profLbl = new QLabel("TX Profile:");
        applyLabelStyle(profLbl);
        grid->addWidget(profLbl, 3, 0);
        profCmb->setAccessibleName(QStringLiteral("TX profile"));
        profCmb->setMinimumWidth(180);
        grid->addWidget(profCmb, 3, 1, Qt::AlignLeft);
        grid->addWidget(bandSetBtn, 4, 1, Qt::AlignLeft);

        auto* optionsGroup = new QGroupBox("Options");
        optionsGroup->setLayout(grid);
        vbox->addWidget(optionsGroup);
    }

    vbox->addStretch(1);
    return page;
}
QWidget* RadioSetupDialog::buildPhoneCwTab()
{
    auto& tx = m_model->transmitModel();
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Model header
    {
        auto* hdr = new QHBoxLayout;
        hdr->addStretch(1);
        auto* modelLbl = new QLabel(m_model->model());
        setTone(modelLbl, Tone::Title);
        hdr->addWidget(modelLbl);
        vbox->addLayout(hdr);
    }


    auto mkTogBtn = [&](const QString& text, bool checked) {
        auto* btn = new QPushButton(text);
        btn->setCheckable(true);
        btn->setChecked(checked);
        setButtonRole(btn, ButtonRole::Select);
        return btn;
    };

    // Microphone group
    {
        auto* group = new QGroupBox("Microphone");
        auto* grid = new QGridLayout(group);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(6);
        grid->setColumnStretch(2, 1);

        constexpr int kMicControlButtonWidth = 104;
        auto addMicRow = [&](int row, const QString& labelText, QPushButton* button) {
            auto* label = new QLabel(labelText);
            applyLabelStyle(label);
            label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            button->setMinimumWidth(kMicControlButtonWidth);
            button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            grid->addWidget(label, row, 0);
            grid->addWidget(button, row, 1);
        };

        auto* biasBtn = mkTogBtn("BIAS", tx.micBias());
        connect(biasBtn, &QPushButton::toggled, this, [this](bool on) {
            m_model->transmitModel().setMicBias(on);
        });
        auto* boostBtn = mkTogBtn("+20dB", tx.micBoost());
        connect(boostBtn, &QPushButton::toggled, this, [this](bool on) {
            m_model->transmitModel().setMicBoost(on);
        });
        auto* metBtn = mkTogBtn(tx.metInRx() ? "Enabled" : "Disabled", tx.metInRx());
        connect(metBtn, &QPushButton::toggled, this, [this, metBtn](bool on) {
            metBtn->setText(on ? "Enabled" : "Disabled");
            m_model->sendCommand(QString("transmit set met_in_rx=%1").arg(on ? 1 : 0));
        });

        addMicRow(0, "Mic Bias Voltage:", biasBtn);
        addMicRow(1, "Mic +20 dB Boost:", boostBtn);
        addMicRow(2, "Level Meter During Receive:", metBtn);

        vbox->addWidget(group);
    }

    // CW group
    {
        auto* group = new QGroupBox("CW");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        // The keyer controls commit through TransmitModel setters, never raw
        // wire text: the setter updates the model and emits phoneStateChanged
        // before sending, which is the only update on a backend that never
        // echoes `iambic`/`iambic_mode` (parsed only in
        // FlexBackend::decodeTransmitState) (#5256).
        // Iambic: Enabled | A | B
        auto* iamLbl = new QLabel("Iambic:");
        applyLabelStyle(iamLbl);
        grid->addWidget(iamLbl, 0, 0);
        auto* iamBtn = mkTogBtn(tx.cwIambic() ? "Enabled" : "Disabled", tx.cwIambic());
        connect(iamBtn, &QPushButton::toggled, this, [this, iamBtn](bool on) {
            iamBtn->setText(on ? "Enabled" : "Disabled");
            m_model->transmitModel().setCwIambic(on);
        });
        // Each row's related buttons share one cell, so they sit together
        // whatever their widths; the second label column starts after them.
        auto* iambicRow = new QHBoxLayout;
        iambicRow->setSpacing(6);
        iambicRow->addWidget(iamBtn);
        grid->addLayout(iambicRow, 0, 1);
        auto* modeA = mkTogBtn("A", tx.cwIambicMode() == 0);
        auto* modeB = mkTogBtn("B", tx.cwIambicMode() == 1);
        connect(modeA, &QPushButton::clicked, this, [this, modeA, modeB] {
            modeA->setChecked(true); modeB->setChecked(false);
            m_model->transmitModel().setCwIambicMode(0);
        });
        connect(modeB, &QPushButton::clicked, this, [this, modeA, modeB] {
            modeA->setChecked(false); modeB->setChecked(true);
            m_model->transmitModel().setCwIambicMode(1);
        });
        iambicRow->addWidget(modeA);
        iambicRow->addWidget(modeB);

        // Swap: Dot/Dash button
        auto* swapLbl = new QLabel("Swap:");
        applyLabelStyle(swapLbl);
        grid->addWidget(swapLbl, 0, 2);
        auto* swapBtn = mkTogBtn("Dot/Dash", tx.cwSwapPaddles());
        connect(swapBtn, &QPushButton::toggled, this, [this](bool on) {
            m_model->transmitModel().setCwSwapPaddles(on);
        });
        grid->addWidget(swapBtn, 0, 3);

        // Sideband: CWU | CWL
        auto* sbLbl = new QLabel("Sideband:");
        applyLabelStyle(sbLbl);
        grid->addWidget(sbLbl, 1, 0);
        auto* cwuBtn = mkTogBtn("CWU", !tx.cwlEnabled());
        auto* cwlBtn = mkTogBtn("CWL", tx.cwlEnabled());
        connect(cwuBtn, &QPushButton::clicked, this, [this, cwuBtn, cwlBtn] {
            cwuBtn->setChecked(true); cwlBtn->setChecked(false);
            m_model->transmitModel().setCwlEnabled(false);
        });
        connect(cwlBtn, &QPushButton::clicked, this, [this, cwuBtn, cwlBtn] {
            cwuBtn->setChecked(false); cwlBtn->setChecked(true);
            m_model->transmitModel().setCwlEnabled(true);
        });
        auto* sidebandRow = new QHBoxLayout;
        sidebandRow->setSpacing(6);
        sidebandRow->addWidget(cwuBtn);
        sidebandRow->addWidget(cwlBtn);
        grid->addLayout(sidebandRow, 1, 1);

        // CWX: Sync
        auto* cwxLbl = new QLabel("CWX:");
        applyLabelStyle(cwxLbl);
        grid->addWidget(cwxLbl, 1, 2);
        auto* syncBtn = mkTogBtn("Sync", tx.syncCwx());
        connect(syncBtn, &QPushButton::toggled, this, [this](bool on) {
            m_model->sendCommand(QString("cw synccwx %1").arg(on ? 1 : 0));
        });
        grid->addWidget(syncBtn, 1, 3);

        // CW Decode — independent RX / TX toggles (#2417).  RX keeps the
        // legacy behaviour of decoding the received CW slice; TX decodes
        // the operator's own keying via the client-side sidetone, useful
        // as a self-training tool for paddle / bug timing.  MainWindow
        // re-evaluates run state and the AudioEngine TX-decode tap on
        // dialog close via refreshCwDecodeState().
        auto* decodeLbl = new QLabel("Decode:");
        applyLabelStyle(decodeLbl);
        grid->addWidget(decodeLbl, 2, 2);
        auto* rxDecodeBtn = mkTogBtn("RX", CwDecodeSettings::rxEnabled());
        auto* txDecodeBtn = mkTogBtn("TX", CwDecodeSettings::txEnabled());
        connect(rxDecodeBtn, &QPushButton::toggled, this, [](bool on) {
            CwDecodeSettings::setRxEnabled(on);
        });
        connect(txDecodeBtn, &QPushButton::toggled, this, [](bool on) {
            CwDecodeSettings::setTxEnabled(on);
        });
        auto* decodeRow = new QHBoxLayout;
        decodeRow->setSpacing(6);
        decodeRow->addWidget(rxDecodeBtn);
        decodeRow->addWidget(txDecodeBtn);
        grid->addLayout(decodeRow, 2, 3);



        vbox->addWidget(group);
    }

    // Digital group
    {
        auto* group = new QGroupBox("Digital");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        auto* markLbl = new QLabel("RTTY Mark Default:");
        applyLabelStyle(markLbl);
        grid->addWidget(markLbl, 0, 0);
        auto* markEdit = new QLineEdit(QString::number(m_model->rttyMarkDefault()));
        markEdit->setFixedWidth(60);
        connect(markEdit, &QLineEdit::editingFinished, this, [this, markEdit] {
            m_model->sendCommand(
                "radio set rtty_mark_default=" + markEdit->text());
        });
        grid->addWidget(markEdit, 0, 1);

        // RTTY Decode — the operator's explicit "I want the decoder pane"
        // state (#5353).  The pane's own ✕ clears this flag, and before it
        // existed there was no way back on: visibility was recomputed from
        // the slice mode, so the pane reopened on the next slice switch or
        // rtty_mark echo.  This is the re-enable, and it mirrors the CW
        // Decode RX/TX toggles above.  MainWindow re-evaluates panel and
        // run state on dialog close via refreshRttyDecodeState().
        auto* rttyDecodeLbl = new QLabel("RTTY Decode:");
        setTone(rttyDecodeLbl, Tone::Caption);
        grid->addWidget(rttyDecodeLbl, 1, 0);
        auto* rttyDecodeBtn = mkTogBtn(
            RttyDecodeSettings::enabled() ? "Enabled" : "Disabled",
            RttyDecodeSettings::enabled());
        rttyDecodeBtn->setObjectName("rttyDecodeEnabled");
        rttyDecodeBtn->setAccessibleName("RTTY Decode");
        rttyDecodeBtn->setAccessibleDescription("Enable or disable the RTTY decoder pane.");
        connect(rttyDecodeBtn, &QPushButton::toggled, this, [rttyDecodeBtn](bool on) {
            RttyDecodeSettings::setEnabled(on);
            rttyDecodeBtn->setText(on ? "Enabled" : "Disabled");
        });
        grid->addWidget(rttyDecodeBtn, 1, 1);

        vbox->addWidget(group);
    }

    vbox->addStretch(1);
    return page;
}
QWidget* RadioSetupDialog::buildRxTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Model header
    {
        auto* hdr = new QHBoxLayout;
        hdr->addStretch(1);
        auto* modelLbl = new QLabel(m_model->model());
        setTone(modelLbl, Tone::Title);
        hdr->addWidget(modelLbl);
        vbox->addLayout(hdr);
    }


    // Frequency Offset group
    {
        auto* group = new QGroupBox("Frequency Offset");
        auto* gvb = new QVBoxLayout(group);
        gvb->setSpacing(4);

        auto* lbl = new QLabel(m_model->gpsdoPresent()
            ? "GPSDO installed. Manual frequency offset calibration available."
            : "Manual frequency offset calibration available.");
        setTone(lbl, m_model->gpsdoPresent() ? Tone::Ok : Tone::Warn);
        lbl->setWordWrap(true);
        gvb->addWidget(lbl);

        auto* offsetGrid = new QGridLayout;
        offsetGrid->setSpacing(6);

        // Cal Frequency row
        auto* calLbl = new QLabel("Cal Frequency (MHz):");
        applyLabelStyle(calLbl);
        offsetGrid->addWidget(calLbl, 0, 0);
        auto* calEdit = new QLineEdit(QString::number(m_model->calFreqMhz(), 'f', 6));
        calEdit->setFixedWidth(100);
        connect(calEdit, &QLineEdit::editingFinished, this, [this, calEdit] {
            m_model->sendCommand(
                "radio set cal_freq=" + calEdit->text());
        });
        offsetGrid->addWidget(calEdit, 0, 1);

        auto* startBtn = new QPushButton("Start");
        startBtn->setFixedWidth(60);
        auto* calStatus = new QLabel;
        setTone(calStatus, Tone::Hint);
        calStatus->setMinimumWidth(130);

        auto calibrationActive = std::make_shared<bool>(false);
        auto pllRunningSeen = std::make_shared<bool>(false);
        auto calibrationRun = std::make_shared<int>(0);
        auto setCalStatus = [calStatus](const QString& text, Tone tone) {
            calStatus->setText(text);
            setTone(calStatus, tone);
        };

        connect(startBtn, &QPushButton::clicked, this,
                [this, calEdit, startBtn, calStatus, calibrationActive, pllRunningSeen,
                 calibrationRun, setCalStatus] {
            const QString calFreq = calEdit->text().trimmed();
            if (calFreq.isEmpty()) {
                setCalStatus("Enter cal frequency", Tone::Warn);
                return;
            }

            const int runId = ++(*calibrationRun);
            *calibrationActive = true;
            *pllRunningSeen = false;
            startBtn->setEnabled(false);
            startBtn->setText("Busy");
            setCalStatus("Starting...", Tone::Hint);

            qCDebug(lcProtocol) << "RadioSetupDialog: frequency calibration requested"
                                 << "cal_freq=" << calFreq
                                 << "reset_freq_error_ppb=0"
                                 << "run_id=" << runId;
            m_model->sendCommand("radio set cal_freq=" + calFreq);
            m_model->sendCommand("radio set freq_error_ppb=0");

            QPointer<RadioSetupDialog> dialog(this);
            QPointer<QPushButton> startGuard(startBtn);
            QPointer<QLabel> statusGuard(calStatus);
            // FlexLib's StartOffsetEnabled=false path starts calibration with radio pll_start.
            m_model->sendCmdPublic("radio pll_start",
                [dialog, startGuard, statusGuard, calibrationActive, calibrationRun, runId](
                    int code, const QString& body) {
                if (!dialog)
                    return;
                QMetaObject::invokeMethod(dialog, [dialog, startGuard, statusGuard,
                                                   calibrationActive, calibrationRun, runId,
                                                   code, body] {
                    if (!dialog || !startGuard || !statusGuard)
                        return;
                    if (!*calibrationActive || *calibrationRun != runId) {
                        qCDebug(lcProtocol)
                            << "RadioSetupDialog: ignoring radio pll_start response for inactive calibration"
                            << "run_id=" << runId
                            << "current_run_id=" << *calibrationRun
                            << "code" << code << "body:" << body;
                        return;
                    }
                    if (code == 0) {
                        statusGuard->setText("Calibrating...");
                        setTone(statusGuard, Tone::Accent);
                        qCDebug(lcProtocol)
                            << "RadioSetupDialog: radio pll_start accepted";
                        return;
                    }

                    *calibrationActive = false;
                    startGuard->setEnabled(true);
                    startGuard->setText("Start");
                    statusGuard->setText(QString("Error 0x%1")
                        .arg(code, 0, 16).toUpper());
                    setTone(statusGuard, Tone::Error);
                    qCWarning(lcProtocol)
                        << "RadioSetupDialog: radio pll_start failed: code"
                        << Qt::hex << code << "body:" << body;
                });
            });

            QTimer::singleShot(20000, this,
                [startGuard, statusGuard, calibrationActive, calibrationRun, runId] {
                if (!startGuard || !statusGuard || !*calibrationActive
                    || *calibrationRun != runId)
                    return;
                *calibrationActive = false;
                startGuard->setEnabled(true);
                startGuard->setText("Start");
                statusGuard->setText("No response");
                setTone(statusGuard, Tone::Error);
                qCWarning(lcProtocol)
                    << "RadioSetupDialog: frequency calibration timed out waiting for pll_done=1"
                    << "run_id=" << runId;
            });
        });
        offsetGrid->addWidget(startBtn, 0, 2);
        offsetGrid->addWidget(calStatus, 0, 3);

        // Freq Error PPB row
        auto* ppbLbl = new QLabel("Freq Offset:");
        applyLabelStyle(ppbLbl);
        offsetGrid->addWidget(ppbLbl, 1, 0);
        auto* ppbEdit = new QLineEdit(QString::number(m_model->freqErrorPpb()));
        ppbEdit->setFixedWidth(80);
        connect(ppbEdit, &QLineEdit::editingFinished, this, [this, ppbEdit] {
            m_model->sendCommand(
                "radio set freq_error_ppb=" + ppbEdit->text());
        });
        ppbEdit->setAccessibleName(QStringLiteral("Frequency offset in ppb"));
        // The unit follows the field, as "ms" does on the Audio page.
        auto* ppbRow = new QHBoxLayout;
        ppbRow->setProperty("rsPacked", true);
        ppbRow->setSpacing(6);
        ppbRow->addWidget(ppbEdit);
        auto* ppbUnitLbl = new QLabel("ppb");
        applyLabelStyle(ppbUnitLbl);
        ppbRow->addWidget(ppbUnitLbl);
        offsetGrid->addLayout(ppbRow, 1, 1, Qt::AlignLeft);
        offsetGrid->setColumnStretch(3, 1);
        gvb->addLayout(offsetGrid);

        connect(m_model, &RadioModel::infoChanged, this, [this, calEdit, ppbEdit] {
            if (!calEdit->hasFocus())
                calEdit->setText(QString::number(m_model->calFreqMhz(), 'f', 6));
            if (!ppbEdit->hasFocus())
                ppbEdit->setText(QString::number(m_model->freqErrorPpb()));
        });

        connect(m_model, &RadioModel::statusReceived, this,
                [startBtn, calStatus, calibrationActive, pllRunningSeen](
                    const QString& object, const QMap<QString, QString>& kvs) {
            if (object != QStringLiteral("radio") || !kvs.contains(QStringLiteral("pll_done")))
                return;

            const QString pllDone = kvs.value(QStringLiteral("pll_done"));
            const QString freqError = kvs.value(QStringLiteral("freq_error_ppb"));
            const QString calFreq = kvs.value(QStringLiteral("cal_freq"));
            qCDebug(lcProtocol)
                << "RadioSetupDialog: frequency calibration status"
                << "pll_done=" << pllDone
                << "freq_error_ppb=" << freqError
                << "cal_freq=" << calFreq
                << "active=" << *calibrationActive
                << "running_seen=" << *pllRunningSeen;

            if (!*calibrationActive)
                return;

            if (pllDone == QStringLiteral("0")) {
                *pllRunningSeen = true;
                calStatus->setText("Calibrating...");
                setTone(calStatus, Tone::Accent);
                return;
            }

            if (pllDone == QStringLiteral("1") && !*pllRunningSeen) {
                qCDebug(lcProtocol)
                    << "RadioSetupDialog: ignoring pll_done=1 before pll_done=0 for active calibration"
                    << "freq_error_ppb=" << freqError
                    << "cal_freq=" << calFreq;
                return;
            }

            if (pllDone == QStringLiteral("1")) {
                *calibrationActive = false;
                startBtn->setEnabled(true);
                startBtn->setText("Start");
                calStatus->setText(freqError.isEmpty()
                    ? QStringLiteral("Complete")
                    : QStringLiteral("Complete (%1 ppb)").arg(freqError));
                setTone(calStatus, Tone::Ok);
                qCDebug(lcProtocol)
                    << "RadioSetupDialog: frequency calibration completed"
                    << "freq_error_ppb=" << freqError
                    << "cal_freq=" << calFreq;
            }
        });

        vbox->addWidget(group);
    }

    // 10 MHz Reference group
    {
        auto* group = new QGroupBox("10 MHz Reference");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        auto* srcLbl = new QLabel("Source:");
        applyLabelStyle(srcLbl);
        grid->addWidget(srcLbl, 0, 0);

        auto* srcCmb = new QComboBox;
        AetherSDR::applyComboStyle(srcCmb);
        refreshOscillatorSourceCombo(srcCmb, m_model);
        connect(srcCmb, &QComboBox::currentIndexChanged, this, [this, srcCmb](int i) {
            m_model->sendCommand(
                "radio oscillator " + srcCmb->itemData(i).toString());
        });
        grid->addWidget(srcCmb, 0, 1);

        // Lock status
        auto* lockLbl = new QLabel(oscillatorStatusText(m_model));
        setTone(lockLbl, oscillatorStatusTone(m_model));
        grid->addWidget(lockLbl, 0, 2);

        // Live-update oscillator status when radio state changes (#967)
        connect(m_model, &RadioModel::oscillatorChanged, this, [this, srcCmb, lockLbl] {
            lockLbl->setText(oscillatorStatusText(m_model));
            setTone(lockLbl, oscillatorStatusTone(m_model));

            const QString current = normalizedOscillatorValue(srcCmb->currentData().toString());
            QSignalBlocker blocker(srcCmb);
            refreshOscillatorSourceCombo(srcCmb, m_model, current);
        });

        vbox->addWidget(group);
    }

    // General RX settings
    {
        auto* grid = new QGridLayout;
        grid->setSpacing(6);

        auto addToggle = [&](int row, const QString& label, bool checked,
                              const QString& cmd) {
            auto* lbl = new QLabel(label);
            applyLabelStyle(lbl);
            grid->addWidget(lbl, row, 0);
            auto* btn = new QPushButton(checked ? "Enabled" : "Disabled");
            btn->setCheckable(true);
            btn->setChecked(checked);
            connect(btn, &QPushButton::toggled, this, [this, cmd, btn](bool on) {
                btn->setText(on ? "Enabled" : "Disabled");
                m_model->sendCommand(
                    QString("%1=%2").arg(cmd).arg(on ? 1 : 0));
            });
            grid->addWidget(btn, row, 1);
        };

        addToggle(0, "Mute local audio when remote:", m_model->muteLocalWhenRemote(),
                  "radio set mute_local_audio_when_remote");
        addToggle(1, "Binaural audio:", m_model->binauralRx(),
                  "radio set binaural_rx");

        auto* audioGroup = new QGroupBox("Audio");
        audioGroup->setLayout(grid);
        vbox->addWidget(audioGroup);
    }

    vbox->addStretch(1);
    return page;
}
// ── Calibration tab ──────────────────────────────────────────────────────────

QWidget* RadioSetupDialog::buildCalibrationTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);



    auto* group = new QGroupBox("Frequency Calibration");
    auto* gvb = new QVBoxLayout(group);
    gvb->setSpacing(8);

    {
        auto* intro = new QLabel(
            "This radio tunes from a free-running crystal and cannot measure its own error, "
            "so the correction is applied here. Receive a signal of known frequency, tune "
            "until it zero-beats, and press Calibrate — or enter an error you already know.");
        applyLabelStyle(intro);
        intro->setWordWrap(true);
        gvb->addWidget(intro);

        auto* warmup = new QLabel(
            "Let the radio warm up for 15 minutes first. The oscillator drifts as it heats.");
        setTone(warmup, Tone::Warn);
        warmup->setWordWrap(true);
        gvb->addWidget(warmup);
    }

    // The calibration is stored against the radio's own identity (its MAC), and
    // that identity does not exist until a radio has been connected this
    // session. Hl2Backend refuses to persist without one — an empty radio_id is
    // the family-wide default row, which every other HL2 would then inherit
    // (AGENTS.md) — and the `freqcal` bridge verb returns an error for the same
    // reason. The page has to say so as well, or the operator trims, watches the
    // readout move, and finds nothing was saved. m_calibrationReseed below
    // disables the controls alongside this label.
    auto* noRadioLbl = new QLabel(
        "Connect the radio first. The calibration belongs to one physical radio, "
        "and there is no radio identity to store it against yet.");
    setTone(noRadioLbl, Tone::Error);
    noRadioLbl->setWordWrap(true);
    noRadioLbl->setVisible(false);
    gvb->addWidget(noRadioLbl);

    auto* grid = new QGridLayout;
    grid->setSpacing(6);
    // Keep the controls next to their labels instead of letting the value
    // column absorb the dialog's full width. Column 3 takes the slack.
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 0);
    grid->setColumnStretch(2, 0);
    grid->setColumnStretch(3, 1);
    int row = 0;

    // ── Reference ────────────────────────────────────────────────────────────
    auto* refLbl = new QLabel("Reference:");
    applyLabelStyle(refLbl);
    grid->addWidget(refLbl, row, 0);

    auto* refCombo = new QComboBox;
    AetherSDR::applyComboStyle(refCombo);
    // Standards first, then Custom. A bench GPSDO or a signal generator locked
    // to one is the BEST reference available — no ionospheric Doppler (which
    // wanders ~10 ppb at 10 MHz on an HF path), no fading mid-null — so Custom
    // is a first-class entry here, not a fallback.
    refCombo->addItem(QStringLiteral("WWV / WWVH — 10 MHz"), 10'000'000.0);
    refCombo->addItem(QStringLiteral("WWV / WWVH — 5 MHz"), 5'000'000.0);
    refCombo->addItem(QStringLiteral("WWV / WWVH — 15 MHz"), 15'000'000.0);
    refCombo->addItem(QStringLiteral("WWV / WWVH — 20 MHz"), 20'000'000.0);
    refCombo->addItem(QStringLiteral("WWV / WWVH — 25 MHz"), 25'000'000.0);
    refCombo->addItem(QStringLiteral("WWV / WWVH — 2.5 MHz"), 2'500'000.0);
    refCombo->addItem(QStringLiteral("CHU — 7.850 MHz"), 7'850'000.0);
    refCombo->addItem(QStringLiteral("CHU — 3.330 MHz"), 3'330'000.0);
    refCombo->addItem(QStringLiteral("CHU — 14.670 MHz"), 14'670'000.0);
    refCombo->addItem(QStringLiteral("GPSDO / signal generator (custom)"), 0.0);
    refCombo->setToolTip(
        QStringLiteral("The true frequency of the signal you are zero-beating.\n"
                       "A local GPSDO is the most accurate choice — attenuate it "
                       "at least 30 dB or couple loosely, its output will overload "
                       "the receiver."));
    refCombo->setMinimumWidth(230);
    grid->addWidget(refCombo, row, 1);

    auto* customEdit = new QLineEdit(QStringLiteral("10.000000"));
    customEdit->setFixedWidth(110);
    customEdit->setValidator(new QDoubleValidator(0.1, 38.4, 6, customEdit));
    customEdit->setToolTip(QStringLiteral("Reference frequency in MHz"));
    customEdit->setVisible(false);
    grid->addWidget(customEdit, row, 2);
    ++row;

    auto referenceHz = [refCombo, customEdit]() -> double {
        const double fixed = refCombo->currentData().toDouble();
        if (fixed > 0.0)
            return fixed;
        // Parse with the same locale the QDoubleValidator accepts. Validating
        // system-locale ("10,000000" on de_DE) and parsing C-locale would let a
        // visibly-valid field read back as 0, and Calibrate would report an
        // empty reference over a field that plainly contains one.
        bool ok = false;
        const double mhz = QLocale::system().toDouble(customEdit->text(), &ok);
        if (!ok)
            return 0.0;
        return mhz * 1.0e6;
    };
    connect(refCombo, &QComboBox::currentIndexChanged, this, [refCombo, customEdit](int) {
        customEdit->setVisible(refCombo->currentData().toDouble() <= 0.0);
    });

    // ── Error, in ppb — the ONE stored number ────────────────────────────────
    auto* ppbLbl = new QLabel("Error:");
    applyLabelStyle(ppbLbl);
    grid->addWidget(ppbLbl, row, 0);

    auto* ppbSpin = new QSpinBox;
    ppbSpin->setObjectName(QStringLiteral("hl2FreqCalPpb"));
    ppbSpin->setRange(Hl2FreqCal::kMinPpb, Hl2FreqCal::kMaxPpb);
    ppbSpin->setSingleStep(10);
    ppbSpin->setSuffix(QStringLiteral(" ppb"));
    ppbSpin->setFixedWidth(120);
    ppbSpin->setAccessibleName(QStringLiteral("Frequency error in parts per billion"));
    ppbSpin->setToolTip(
        QStringLiteral("Positive = the radio's clock runs fast, so signals appear low.\n"
                       "100 ppb is 1 Hz at 10 MHz."));
    ppbSpin->setValue(Hl2FreqCal::loadPpb(m_model->settingsScope()));
    grid->addWidget(ppbSpin, row, 1);

    auto* resetBtn = new QPushButton("Reset");
    resetBtn->setFixedWidth(70);
    resetBtn->setToolTip(QStringLiteral("Return this radio to uncalibrated (0 ppb)"));
    grid->addWidget(resetBtn, row, 2);
    ++row;

    // The single write path for the page: adopt into the backend (which
    // persists, re-pushes every NCO, and moves transmit with them) and refresh
    // the readout. Everything below drives the spinbox; only this applies it.
    auto* readout = new QLabel;
    readout->setObjectName(QStringLiteral("hl2FreqCalReadout"));
    readout->setWordWrap(true);
    setTone(readout, Tone::Value);

    auto activeSliceHz = [this]() -> double {
        for (SliceModel* s : m_model->slices()) {
            if (s && s->isActive())
                return s->frequency() * 1.0e6;   // SliceModel carries MHz
        }
        return 0.0;
    };

    auto refreshReadout = [readout, ppbSpin, activeSliceHz] {
        const int ppb = ppbSpin->value();
        const double clock = Hl2FreqCal::effectiveClockHz(ppb);
        QString text = QStringLiteral("Effective clock %1 Hz  ·  %2 ppb (%3 ppm)")
            .arg(QLocale::system().toString(qRound64(clock)))
            .arg(ppb)
            .arg(ppb / 1000.0, 0, 'f', 3);
        // The line that makes ppb mean something. "-182 ppb" is abstract;
        // "-5.2 Hz at 28.500 MHz" is the error the operator was looking at.
        if (const double rf = activeSliceHz(); rf > 0.0) {
            text += QStringLiteral("\nWithout this correction, %1 MHz would be off by %2 Hz.")
                .arg(rf / 1.0e6, 0, 'f', 6)
                .arg(Hl2FreqCal::errorHzAt(rf, ppb), 0, 'f', 2);
        }
        readout->setText(text);
    };

    // persist=false applies live without writing the settings store — used by
    // the auto-repeating Trim buttons, which commit once on release.
    //
    // WITH A REQUEST ID, NOT ZERO. The backend refuses a non-zero correction
    // under a locked CL1 reference and states why — through extensionError,
    // which it emits only for a caller that gave it an id. This page used to
    // write with id 0, so the refusal went nowhere and the spin box went on
    // showing a correction the radio had never taken (@on8st, #5923 review).
    // The controls are disabled under CL1 now, so this is the second line of
    // defence rather than the first: it is what still covers CL1 being switched
    // on by the bridge while this page is open, which no reseed has seen yet.
    //
    // refreshReadout() runs BEFORE the write, so the refusal handler below is
    // free to have the last word on the label whether the reply is synchronous
    // (it is today) or not.
    auto apply = [this, ppbSpin, readout, refreshReadout](int ppb, bool persist = true) {
        QSignalBlocker blocker(ppbSpin);
        ppbSpin->setValue(Hl2FreqCal::clampPpb(ppb));
        refreshReadout();
        // ARMED ONLY WHERE AN ANSWER IS GUARANTEED. Hl2Backend replies to every
        // non-zero request id — result or error — on every path including its
        // unknown-verb fallback, so the guard below is always reclaimed. A
        // backend that made no such promise would leak one small QObject per
        // trim tick, so the id stays 0 for anything that does not declare the
        // namespace, which is exactly the behaviour this page had before.
        constexpr quint64 kCalSetRequestIdBase = 0x0500000000000000ull;
        static quint64 nextCalSetId = kCalSetRequestIdBase;
        IRadioBackend* backend = m_model ? m_model->backend() : nullptr;
        const bool canAnswer =
            backend && m_model->backendDeclaresExtension(QStringLiteral("hl2"));
        const quint64 requestId = canAnswer ? nextCalSetId++ : 0;
        if (canAnswer) {
            auto* guard = new QObject(this);
            connect(backend, &IRadioBackend::extensionResult, guard,
                    [guard, requestId](quint64 id, const QVariant&) {
                if (id == requestId) {
                    guard->deleteLater();
                }
            });
            connect(backend, &IRadioBackend::extensionError, guard,
                    [this, guard, requestId, readout](quint64 id, const QString& reason) {
                if (id != requestId) {
                    return;
                }
                guard->deleteLater();
                // Nothing was applied and nothing was stored. Put the radio's
                // own number back — the reseed also re-reads whether the
                // reference is locked, so the controls end up dimmed with the
                // reason on the spin box — and then say why, because a value
                // that quietly reverts is the "reports success while nothing
                // persists" failure this page's own note warns about.
                if (m_calibrationReseed) {
                    m_calibrationReseed();
                }
                readout->setText(readout->text() + QStringLiteral("\n") + reason);
            });
        }
        // The backend owns clamping, persistence and the re-push. Going through
        // it rather than writing settings here is what keeps a mid-session
        // change audible immediately instead of at the next tune.
        m_model->invokeBackendExtension(QStringLiteral("hl2"),
                                        persist ? QStringLiteral("freqcal.set")
                                                : QStringLiteral("freqcal.set_live"),
                                        requestId, QVariant(ppbSpin->value()));
    };

    // Commit on editing-finished rather than on every intermediate keystroke:
    // with keyboard tracking on, typing "-1782" would persist four partial
    // values and command four intermediate frequencies to the radio.
    ppbSpin->setKeyboardTracking(false);
    connect(ppbSpin, &QSpinBox::valueChanged, this, [apply](int v) { apply(v); });
    connect(resetBtn, &QPushButton::clicked, this, [apply] { apply(0); });

    // ── Trim ─────────────────────────────────────────────────────────────────
    //
    // Steps are labelled in Hz-at-10-MHz because that is the unit an operator
    // nulling a beat note is actually hearing. The STORED value stays ppb: the
    // error is fractional, so a step that means 1 Hz on 10 MHz means 0.1 Hz on
    // 160 m, and storing Hz would be right on exactly one band.
    auto* trimLbl = new QLabel("Trim:");
    applyLabelStyle(trimLbl);
    grid->addWidget(trimLbl, row, 0);

    auto* trimRow = new QWidget;
    auto* trimBox = new QHBoxLayout(trimRow);
    trimBox->setContentsMargins(0, 0, 0, 0);
    trimBox->setSpacing(6);

    auto* stepCombo = new QComboBox;
    AetherSDR::applyComboStyle(stepCombo);
    stepCombo->addItem(QStringLiteral("0.1 Hz @ 10 MHz"), 10);
    stepCombo->addItem(QStringLiteral("1 Hz @ 10 MHz"), 100);
    stepCombo->addItem(QStringLiteral("10 Hz @ 10 MHz"), 1000);
    stepCombo->setCurrentIndex(1);

    auto* downBtn = new QPushButton(QStringLiteral("−"));
    auto* upBtn = new QPushButton(QStringLiteral("+"));
    for (QPushButton* b : {downBtn, upBtn}) {
        b->setFixedWidth(44);
        b->setAutoRepeat(true);
        b->setAutoRepeatDelay(400);
        b->setAutoRepeatInterval(120);
    }
    downBtn->setAccessibleName(QStringLiteral("Decrease frequency calibration"));
    upBtn->setAccessibleName(QStringLiteral("Increase frequency calibration"));
    trimBox->addWidget(downBtn);
    trimBox->addWidget(upBtn);
    trimBox->addWidget(stepCombo);
    trimBox->addStretch(1);
    grid->addWidget(trimRow, row, 1, 1, 2);
    ++row;

    // Applied live while held, persisted once on release. Read isDown() inside
    // clicked(), not released(): Qt auto-repeat emits released/clicked/pressed
    // every tick with the button still down, and released() precedes the step,
    // so committing there persists ~8x/s with a stale value
    // (tests/hl2_trim_autorepeat_test.cpp, Qt 6.11).
    auto trim = [apply, ppbSpin, stepCombo](QPushButton* b, int sign) {
        apply(ppbSpin->value() + sign * stepCombo->currentData().toInt(),
              /*persist=*/!b->isDown());
    };
    connect(downBtn, &QPushButton::clicked, this, [trim, downBtn] { trim(downBtn, -1); });
    connect(upBtn, &QPushButton::clicked, this, [trim, upBtn] { trim(upBtn, +1); });

    gvb->addLayout(grid);

    // ── Calibrate from the current VFO ───────────────────────────────────────
    auto* calRow = new QHBoxLayout;
    calRow->setSpacing(8);
    auto* calBtn = new QPushButton("Calibrate from Current VFO");
    calBtn->setObjectName(QStringLiteral("hl2FreqCalCapture"));
    auto* calStatus = new QLabel;
    calStatus->setWordWrap(true);
    calRow->addWidget(calBtn);
    calRow->addWidget(calStatus, 1);
    gvb->addLayout(calRow);

    auto setStatus = [calStatus](const QString& text, Tone tone) {
        calStatus->setText(text);
        setTone(calStatus, tone);
    };

    connect(calBtn, &QPushButton::clicked, this,
            [this, apply, referenceHz, activeSliceHz, setStatus] {
        const double ref = referenceHz();
        if (!(ref > 0.0)) {
            setStatus(QStringLiteral("Enter a reference frequency."), Tone::Warn);
            return;
        }
        const double dialled = activeSliceHz();
        if (!(dialled > 0.0)) {
            setStatus(QStringLiteral("No active slice to read."), Tone::Warn);
            return;
        }
        const int ppb = Hl2FreqCal::ppbFromZeroBeat(ref, dialled);
        // Refuse an implausible result rather than committing it. Zero-beating
        // the wrong signal — a harmonic, the opposite sideband, the wrong
        // station — produces a number that would move every band by kilohertz,
        // and the operator would have no way to tell that from a real reading.
        if (ppb == Hl2FreqCal::kMinPpb || ppb == Hl2FreqCal::kMaxPpb) {
            setStatus(QStringLiteral("That is more than 50 ppm off (%1 MHz vs %2 MHz "
                                     "reference) — check you are on the right signal.")
                          .arg(dialled / 1.0e6, 0, 'f', 6)
                          .arg(ref / 1.0e6, 0, 'f', 6),
                      Tone::Error);
            return;
        }
        apply(ppb);
        setStatus(QStringLiteral("Calibrated: %1 MHz reads as %2 MHz → %3 ppb.")
                      .arg(ref / 1.0e6, 0, 'f', 6)
                      .arg(dialled / 1.0e6, 0, 'f', 6)
                      .arg(ppb),
                  Tone::Ok);
    });

    gvb->addWidget(readout);
    vbox->addWidget(group);

    // Keep the "off by N Hz at this frequency" line honest as the operator
    // tunes around looking for the reference.
    auto trackSlice = [this, refreshReadout](SliceModel* s) {
        if (s)
            connect(s, &SliceModel::frequencyChanged, this,
                    [refreshReadout] { refreshReadout(); });
    };
    for (SliceModel* s : m_model->slices())
        trackSlice(s);
    // Slices created after the page was built (including every slice, when the
    // page was built before connecting) would otherwise never move the "off by
    // N Hz" line — the one line that gives this page its reason to exist.
    connect(m_model, &RadioModel::sliceAdded, this,
            [trackSlice, refreshReadout](SliceModel* s) {
        trackSlice(s);
        refreshReadout();
    });

    // Re-seed from the store, and re-check that there is a radio to store
    // against, whenever the dialog is shown or the connected radio changes. The
    // page is built once per process and the dialog is a persistent singleton,
    // so without this the spinbox would keep the value it read at first build —
    // and the next Trim press would commit that number to whichever radio is
    // connected now.
    QPointer<QSpinBox> spinGuard(ppbSpin);
    QPointer<QLabel> noRadioGuard(noRadioLbl);
    const QList<QPointer<QWidget>> calControls{
        refCombo, customEdit, ppbSpin, resetBtn, downBtn, upBtn, stepCombo, calBtn};
    // Ask the backend whether this radio is on an external reference, and fold
    // the answer into m_hl2ExternalRefLocked. THROUGH THE SEAM, not through the
    // vendor header: the extension reply is the only thing this file may read,
    // the same rule the HL2 Hardware page follows (EB3, see its note).
    //
    // Synchronous in practice — freqcal.get completes inside the backend and
    // emits its reply before invokeBackendExtension() returns, which is why the
    // handler is armed first and why the caller can read the member on the next
    // line. If that ever stops being true the page is one reseed stale rather
    // than wrong, and the live writer below still corrects it.
    auto askExternalReference = [this] {
        IRadioBackend* backend = m_model ? m_model->backend() : nullptr;
        if (!backend || !m_model->backendDeclaresExtension(QStringLiteral("hl2"))) {
            return;
        }
        constexpr quint64 kCalRequestIdBase = 0x0400000000000000ull;
        static quint64 nextCalId = kCalRequestIdBase;
        const quint64 requestId = nextCalId++;
        auto* guard = new QObject(this);
        connect(backend, &IRadioBackend::extensionResult, guard,
                [this, guard, requestId](quint64 id, const QVariant& result) {
            if (id != requestId) {
                return;
            }
            guard->deleteLater();
            const QVariantMap m = result.toMap();
            if (m.contains(QStringLiteral("externalReference"))) {
                m_hl2ExternalRefLocked =
                    m.value(QStringLiteral("externalReference")).toBool();
            }
        });
        connect(backend, &IRadioBackend::extensionError, guard,
                [guard, requestId](quint64 id, const QString&) {
            if (id == requestId) {
                guard->deleteLater();   // leave the page as it stands
            }
        });
        m_model->invokeBackendExtension(QStringLiteral("hl2"),
                                        QStringLiteral("freqcal.get"), requestId);
    };

    m_calibrationReseed = [this, spinGuard, noRadioGuard, calControls, refreshReadout,
                           askExternalReference] {
        if (!spinGuard)
            return;
        {
            QSignalBlocker blocker(spinGuard);
            spinGuard->setValue(Hl2FreqCal::loadPpb(m_model->settingsScope()));
        }
        // No identity, no write — the backend and the bridge verb both refuse in
        // this state, so leaving the controls live would be a UI that reports
        // success while nothing persists.
        const bool haveRadio = !m_model->settingsScope().radioId().isEmpty();
        // AND NOT WHILE THE RADIO IS LOCKED TO CL1. A disciplined 10 MHz
        // reference has already removed the crystal error this page exists to
        // model, so a manual ppb on top of it does not refine the correction —
        // it reintroduces exactly the error that was measured off the old
        // crystal. docs/architecture/hl2-frequency-calibration.md §4 requires
        // the control to be disabled, and the backend refuses the verb as well;
        // this is the half the operator can see.
        //
        // THIS PAGE ASKS FOR ITSELF. It used to read m_hl2ExternalRefLocked and
        // nothing else, and that member has only two writers — both on the HL2
        // Hardware page. Pages are built lazily (addPage() defers every builder
        // but Radio's into m_deferredBuilders), so with CL1 persisted from an
        // earlier session an operator who opens Radio Setup and goes straight to
        // Calibration found every control here ENABLED, and moving one sent
        // freqcal.set with requestId 0 — a request the backend correctly refuses
        // and whose refusal, emitted only `if (requestId != 0)`, went nowhere.
        // The spin box then showed a correction that was never applied or
        // stored, until the next showEvent() put 0 back. This file already names
        // that shape as the thing to avoid: "a UI that reports success while
        // nothing persists" (#5923 review, @on8st).
        //
        // freqcal.get reports externalReference for exactly this reason — its
        // own comment says the answer carries "the reason it is refused in the
        // same answer as the value" — so the page reads it here and stops
        // depending on which page the operator happened to visit first. The
        // member stays as the LIVE path: the HL2 Hardware page's checkbox
        // updates it directly so a mid-session toggle is reflected without a
        // round trip, and this reseed reconciles it on every open.
        askExternalReference();
        const bool locked = m_hl2ExternalRefLocked;
        for (const QPointer<QWidget>& w : calControls) {
            if (w) {
                w->setEnabled(haveRadio && !locked);
            }
        }
        if (spinGuard) {
            spinGuard->setAccessibleDescription(
                locked ? QStringLiteral(
                             "Unavailable: this radio is locked to an external 10 MHz "
                             "reference at CL1, which already removes the crystal's error.")
                       : QString());
            spinGuard->setToolTip(
                locked ? QStringLiteral(
                             "Disabled while the HL2 Hardware page has this radio locked to "
                             "an external 10 MHz reference at CL1 — the reference already "
                             "removes the crystal's error, so a manual correction would "
                             "reintroduce it.")
                       : QString());
        }
        if (noRadioGuard)
            noRadioGuard->setVisible(!haveRadio);
        refreshReadout();
    };
    m_calibrationReseed();

    vbox->addStretch(1);
    return page;
}

// HL2 Hardware tab. A Hermes-Lite 2, an HL2 with the AK4951 board and a
// SquareSDR 2 are identical on the wire (no board ID in Protocol 1) but differ
// in hardware (the dither bit drives a band-voltage output on the bare board
// and the loudspeaker on codec boards). Each control is the operator's
// declaration; Protocol 1 has no readback, so controls show what is stored and
// sent (Hl2HardwareOptions).
QWidget* RadioSetupDialog::buildHl2HardwareTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);


    // kLabelStyleTemplate, NOT a local copy of it. #5898 made that constant this
    // file's single owner of the label style, and a retyped duplicate is how the
    // two drift apart on the next token rename — which is the failure the
    // constant's own comment describes. The other builders' local copies predate
    // #5898 and are not this change's to convert.

    // Collected so the reseed lambda can dim the whole page in one pass when
    // there is no radio identity to store against — same shape, and the same
    // reason, as the Calibration page's calControls.
    auto controls = std::make_shared<QVector<QPointer<QWidget>>>();

    // The write path for the page. ONE function, so no control can persist a
    // change without also pushing it, and the backend stays the only thing that
    // decides what a change means on the wire.
    //
    // Sends only the fields the caller names: hw.set is deliberately partial,
    // so a checkbox cannot clobber a combo the operator set a moment ago.
    auto apply = [this](const QVariantMap& fields) {
        m_model->invokeBackendExtension(QStringLiteral("hl2"),
                                        QStringLiteral("hw.set"), 0, fields);
    };

    auto* noRadioLbl = new QLabel(
        "Connect the radio first. These settings describe one physical radio and are "
        "stored against its identity, which does not exist until it has been connected.");
    setTone(noRadioLbl, Tone::Error);
    noRadioLbl->setWordWrap(true);
    noRadioLbl->setVisible(false);
    vbox->addWidget(noRadioLbl);

    // ── Local audio codec ────────────────────────────────────────────────────
    auto* codecGroup = new QGroupBox("Local Audio");
    auto* cvb = new QVBoxLayout(codecGroup);
    cvb->setSpacing(6);

    auto* codecIntro = new QLabel(
        "A plain Hermes-Lite 2 has no audio codec: the slot the HPSDR protocol reserves for "
        "headphone audio is used by the gateware as an extended-address register, so nothing "
        "may be written there. Two variants do have a codec and do accept audio.");
    setTone(codecIntro, Tone::Hint);
    codecIntro->setWordWrap(true);
    cvb->addWidget(codecIntro);

    auto* codecRow = new QHBoxLayout;
    auto* codecLbl = new QLabel("Codec:");
    applyLabelStyle(codecLbl);
    auto* codecCombo = new QComboBox;
    codecCombo->setObjectName(QStringLiteral("hl2HwCodec"));
    AetherSDR::applyComboStyle(codecCombo);
    codecCombo->addItem(QStringLiteral("None — plain Hermes-Lite 2"), 0);
    codecCombo->addItem(QStringLiteral("AK4951 — HL2+ companion board"), 1);
    codecCombo->addItem(QStringLiteral("SquareSDR 2 — codec on the mainboard"), 2);
    codecCombo->setAccessibleName(QStringLiteral("Local audio codec"));
    codecCombo->setToolTip(QStringLiteral(
        "Which local audio codec this board has.\n\n"
        "None: a plain Hermes-Lite 2. Receive audio plays on the computer only,\n"
        "and the control below is the band-voltage output on the CL2 jack.\n\n"
        "AK4951: the HL2+ companion board. The control below becomes that\n"
        "codec's loudspeaker switch, and starts on — the gateware turns the\n"
        "speaker on by itself at power-up.\n\n"
        "SquareSDR 2: the codec is on the mainboard, and the control below is\n"
        "its internal loudspeaker.\n\n"
        "It is one bit on the wire either way, and it is always yours to set."));
    codecRow->addWidget(codecLbl);
    codecRow->addWidget(codecCombo);
    codecRow->addStretch(1);
    cvb->addLayout(codecRow);
    controls->append(codecCombo);

    // ── The radio's own speaker level ────────────────────────────────────────
    //
    // ITS OWN FADER, and the reason is structural rather than a preference:
    // the application's volume control is a device attenuation applied to the
    // host's audio sink, so it is not in the samples this page's codec feed is
    // taken from and could not be picked up by any amount of tapping. Without
    // this the radio's loudspeaker would sit at whatever the slice faders
    // happen to sum to. See Hl2HardwareOptions::speakerLevelPercent.
    auto* spkRow = new QHBoxLayout;
    auto* spkLbl = new QLabel("Speaker level:");
    applyLabelStyle(spkLbl);
    auto* spkSlider = new QSlider(Qt::Horizontal);
    spkSlider->setObjectName(QStringLiteral("hl2HwSpeakerLevel"));
    spkSlider->setRange(0, 100);
    spkSlider->setMinimumWidth(180);
    spkSlider->setAccessibleName(QStringLiteral("Radio speaker level"));
    spkSlider->setToolTip(QStringLiteral(
        "Level of the radio's own loudspeaker or headphone jack.\n\n"
        "Separate from the application's volume control on purpose: that one is\n"
        "applied to the computer's audio output and never reaches the radio.\n"
        "Per-slice audio faders, balance and mute DO reach it — this rides on\n"
        "top of them. Set it to 0 to silence the radio's speaker."));
    auto* spkValue = new QLabel;
    setTone(spkValue, Tone::Hint);
    spkValue->setMinimumWidth(36);
    spkRow->addWidget(spkLbl);
    spkRow->addWidget(spkSlider, 1);
    spkRow->addWidget(spkValue);
    cvb->addLayout(spkRow);
    controls->append(spkSlider);
    vbox->addWidget(codecGroup);

    // A fader for a speaker that is not there decides nothing. Dimmed with a
    // stated reason rather than hidden, and the reason reaches a screen reader
    // through accessibleDescription — a tooltip is a mouse affordance and is
    // never announced (AGENTS.md, three-state controls).
    auto refreshSpeaker = [spkSlider, spkValue, spkLbl, codecCombo] {
        const bool haveCodec = codecCombo->currentData().toInt() != 0;
        spkSlider->setEnabled(haveCodec);
        spkLbl->setEnabled(haveCodec);
        spkSlider->setAccessibleDescription(
            haveCodec ? QStringLiteral("Level of the radio's own loudspeaker.")
                      : QStringLiteral(
                            "Unavailable: this radio has no local audio codec, so it has "
                            "no speaker of its own to set a level for."));
        spkValue->setText(haveCodec ? QStringLiteral("%1%").arg(spkSlider->value())
                                    : QStringLiteral("—"));
    };

    // ── The dither bit ───────────────────────────────────────────────────────
    //
    // ITS OWN GROUP, not a checkbox tacked onto the codec row, because it is
    // the one control on this page whose MEANING changes with another control.
    // The label and the hint below are rewritten from the selected codec, which
    // is the only way an operator can be expected to know what they are
    // switching.
    auto* ditherGroup = new QGroupBox("Dither Bit");
    auto* dvb = new QVBoxLayout(ditherGroup);
    dvb->setSpacing(6);

    auto* ditherChk = new QCheckBox("Dither bit");
    ditherChk->setObjectName(QStringLiteral("hl2HwDither"));
    dvb->addWidget(ditherChk);
    controls->append(ditherChk);

    auto* ditherHint = new QLabel;
    setTone(ditherHint, Tone::Hint);
    ditherHint->setWordWrap(true);
    dvb->addWidget(ditherHint);

    auto* randomChk = new QCheckBox("Random bit");
    randomChk->setObjectName(QStringLiteral("hl2HwRandom"));
    randomChk->setToolTip(QStringLiteral(
        "The companion of the dither bit on the original openHPSDR hardware.\n"
        "No Hermes-Lite 2 variant is known to use it for anything; offered for\n"
        "parity with other HPSDR clients."));
    dvb->addWidget(randomChk);
    controls->append(randomChk);
    vbox->addWidget(ditherGroup);

    // Rewrites the dither control from the codec: three meanings, one bit.
    // PRESENTATION ONLY — this never moves the checkbox. It used to force it
    // checked for an AK4951, on the belief that the gateware read the bit as
    // "a codec is present"; it does not (see Hl2HardwareOptions::Codec for the
    // RTL). Keeping the write out of here is also what lets every caller stop
    // worrying about signal blockers: a refresh that only relabels cannot
    // persist an intent the operator never expressed.
    auto refreshDither = [ditherChk, ditherHint, codecCombo] {
        const int codec = codecCombo->currentData().toInt();
        switch (codec) {
        case 1:   // AK4951
            ditherChk->setText(QStringLiteral("Internal loudspeaker"));
            ditherChk->setEnabled(true);
            ditherChk->setAccessibleDescription(QStringLiteral(
                "Switches the AK4951 companion board's loudspeaker on or off."));
            ditherHint->setText(QStringLiteral(
                "On the HL2+ companion board this bit switches the AK4951's "
                "loudspeaker. The gateware turns it on at power-up, so declaring "
                "the board starts it checked; clearing it is what silences the "
                "radio's own speaker."));
            break;
        case 2:   // SquareSDR 2
            ditherChk->setText(QStringLiteral("Internal loudspeaker"));
            ditherChk->setEnabled(true);
            ditherChk->setAccessibleDescription(QStringLiteral(
                "Switches the SquareSDR 2's internal loudspeaker on or off."));
            ditherHint->setText(QStringLiteral(
                "On the SquareSDR 2 this bit switches the internal loudspeaker. "
                "It is the same bit a Hermes-Lite 2 uses for its band-voltage output."));
            break;
        default:  // bare HL2
            ditherChk->setText(QStringLiteral("Band voltage output"));
            ditherChk->setEnabled(true);
            ditherChk->setAccessibleDescription(QStringLiteral(
                "Enables the Hermes-Lite 2's band-voltage output on the CL2 jack."));
            ditherHint->setText(QStringLiteral(
                "The Hermes-Lite 2 gateware uses this bit, undocumented, to drive a "
                "band-dependent DC level on the CL2 jack — the \"band volts\" output."));
            break;
        }
    };

    // ── Companion filter board ───────────────────────────────────────────────
    auto* filterGroup = new QGroupBox("Companion Filter Board");
    auto* fvb = new QVBoxLayout(filterGroup);
    fvb->setSpacing(6);

    auto* filterIntro = new QLabel(
        "The Hermes-Lite 2 has no filters of its own — it drives seven open-collector "
        "outputs that a board on J16 decodes. Where that board sits in the signal path "
        "decides whether receive is filtered too.");
    setTone(filterIntro, Tone::Hint);
    filterIntro->setWordWrap(true);
    fvb->addWidget(filterIntro);

    auto* filterRow = new QHBoxLayout;
    auto* filterLbl = new QLabel("Board:");
    applyLabelStyle(filterLbl);
    auto* filterCombo = new QComboBox;
    filterCombo->setObjectName(QStringLiteral("hl2HwFilterBoard"));
    AetherSDR::applyComboStyle(filterCombo);
    filterCombo->addItem(QStringLiteral("None — nothing on J16"), 0);
    filterCombo->addItem(QStringLiteral("N2ADR — receive and transmit"), 1);
    filterCombo->addItem(QStringLiteral("N2ADR — transmit only"), 2);
    filterCombo->setAccessibleName(QStringLiteral("Companion filter board"));
    filterCombo->setToolTip(QStringLiteral(
        "None: no board fitted. Every relay is released — the front end is bare\n"
        "on receive, and nothing filters the transmitter's harmonics on the way\n"
        "to the antenna. Choose it only when there really is nothing on J16.\n\n"
        "Receive and transmit: the usual boxed Hermes-Lite 2 — the band's low-pass\n"
        "and the AM-broadcast high-pass are ahead of the ADC as well as after the PA.\n\n"
        "Transmit only: the low-pass bank is in the transmit path alone. This is the\n"
        "SquareSDR 2's arrangement, and also an HL2 whose filter board sits between\n"
        "the PA and the antenna. Receive then sees the bare front end unless you add\n"
        "the 3 MHz high-pass below."));
    filterRow->addWidget(filterLbl);
    filterRow->addWidget(filterCombo);
    filterRow->addStretch(1);
    fvb->addLayout(filterRow);
    controls->append(filterCombo);

    auto* hpfChk = new QCheckBox("Receive through the N2ADR 3 MHz high-pass");
    hpfChk->setObjectName(QStringLiteral("hl2HwN2adrHpf"));
    hpfChk->setToolTip(QStringLiteral(
        "Engages the board's AM-broadcast high-pass on receive only, on 80 m through\n"
        "10 m. It stays out on 160 m — the radio's own switching supply couples spurs\n"
        "into that filter — and below 1.6 MHz, where it would remove what you are\n"
        "listening to."));
    fvb->addWidget(hpfChk);
    controls->append(hpfChk);
    vbox->addWidget(filterGroup);

    // The high-pass belongs to the transmit-only wiring: with the board in the
    // receive path the filter rides the per-band pattern and this control
    // decides nothing. Dimmed with a reason rather than hidden.
    auto refreshHpf = [hpfChk, filterCombo] {
        const bool txOnly = filterCombo->currentData().toInt() == 2;
        hpfChk->setEnabled(txOnly);
        hpfChk->setAccessibleDescription(
            txOnly ? QStringLiteral("Adds the board's 3 MHz high-pass to the receive path.")
                   : QStringLiteral(
                         "Unavailable: this only applies when the filter board is in the "
                         "transmit path alone. With the board on receive as well, the "
                         "high-pass already rides the per-band filter selection."));
    };

    // ── Reference clock and tuner ────────────────────────────────────────────
    auto* miscGroup = new QGroupBox("Reference Clock and Tuner");
    auto* mvb = new QVBoxLayout(miscGroup);
    mvb->setSpacing(6);

    auto* cl1Chk = new QCheckBox("External 10 MHz reference at CL1");
    cl1Chk->setObjectName(QStringLiteral("hl2HwCl1"));
    cl1Chk->setToolTip(QStringLiteral(
        "Reprograms the on-board VersaClock to lock to a 10 MHz reference fed into\n"
        "the CL1 jack — a GPSDO, typically — instead of the radio's own crystal.\n\n"
        "The CL1 input runs through a divider scaled for a 3.3 V logic-level\n"
        "clock, so a hotter source wants padding: a typical GPSDO's 5 V output\n"
        "needs at least 6 dB of 50 Ω attenuation in line. A sine is fine — the\n"
        "part locks to what arrives, it does not need a square wave.\n\n"
        "This is the CLOCK input, not the antenna. Feeding a GPSDO into the RF\n"
        "input to calibrate by ear is a different thing with a different figure —\n"
        "that one needs ≥30 dB, because it reaches the AD9866.\n\n"
        "The register sequence is the Hermes-Lite 2's. It was exercised on an\n"
        "HL2, not on a SQUARE SDR 2 — that board carries the same VersaClock\n"
        "(5P49V5923) and its own CL1 divider, so the same tables should apply,\n"
        "but nobody here has run them on one.\n\n"
        "The radio boots on its crystal every time, so this is re-sent on connect\n"
        "while it is enabled. It also forces the manual frequency calibration to\n"
        "zero — a disciplined reference has no crystal error left to correct."));
    mvb->addWidget(cl1Chk);
    controls->append(cl1Chk);

    // THE ATTENUATOR WARNING IS ON THE PAGE, NOT IN THE TOOLTIP, and that is a
    // requirement rather than a preference: docs/architecture/
    // hl2-frequency-calibration.md §4 says "Any UI that offers this must say
    // so, in the UI, not just the release notes." A tooltip is not in the UI
    // for this purpose — it needs a hover the operator has no reason to make,
    // and the reader who most needs this is the one connecting the cable
    // without hesitating. It is also NOT hidden behind the checkbox's state:
    // by the time the box is ticked the cable is already on.
    //
    // TWO DIFFERENT CAUTIONS LIVE IN THAT SECTION AND THEY MUST NOT BE MERGED.
    // §4 warns about the ANTENNA input — a GPSDO's +7…+13 dBm reaching the
    // AD9866, which has no attenuator ahead of it, needing >=30 dB — and that
    // one already has its home on the Calibration page's reference combo.
    // THIS one is the CL1 jack, a clock input, where the hazard is a 5 V sine
    // against a part expecting 3.3 V square and the figure is >=6 dB. A first
    // draft of this label welded them ("…>=6 dB… or it overloads the AD9866"),
    // which names the wrong port and under-specs the antenna case by ~24 dB.
    // Do not reintroduce the AD9866 here: nothing on CL1 reaches it.
    // EVERY CLAUSE HERE IS THE PROJECT WIKI'S, and the HL2 being open hardware
    // is what makes that checkable rather than a matter of belief
    // (softerhardware/Hermes-Lite2 wiki, External-Clocks):
    //
    //   "Many GPSDOs provide 5V sine wave output whereas HL2 requires a 3.3V
    //    square wave input; a 50 ohm SMA attenuator of at least 6 dB will
    //    reduce the voltage from a 5V source to a level that will not damage
    //    the chip."
    //   "Care must be used to not overdrive the CL1 input to the VersaClock
    //    chip otherwise it may be damaged."
    //
    // 3.3 V IS THE DIVIDER'S DESIGN POINT, NOT A REQUIREMENT, and the wiki's
    // "HL2 requires a 3.3V square wave input" is a simplification worth not
    // reproducing. CL1 goes through B58, an AC coupling cap, then an R39/R40
    // divider scaled so that a 3.3 V LVCMOS clock lands at about 1 Vpp, which
    // is what CLKIN wants. Production boards carry that divider.
    //
    // Two things follow. The part does not "expect 3.3 V" — it needs a usable
    // amplitude at CLKIN, and a source below the design point still works as
    // long as enough arrives. And it does not need a SQUARE wave: a 10 MHz sine
    // fed straight into CL1 with no attenuator lands at roughly a third of its
    // amplitude and the VersaClock locks to it.
    //
    // WHOSE MEASUREMENT: this PR's author (@jcmerg), on a Hermes-Lite 2 beta2
    // board, reported on aethersdr/AetherSDR#5923. Named because "measured on
    // this bench" has no referent to a reader of this file, and because an
    // earlier draft credited it to someone else (@on8st, #5923 review). It is
    // one bench and one board: the divider ratio below is the part of this that
    // a document backs.
    //
    // The divider that does the dividing is R39 = 130 Ω over R40 = 75 Ω in the
    // reference design (hardware/hl/Clock.sch), i.e. 0.366, and the SQUARE
    // SDR 2 uses R75 = 150 Ω over R77 = 75 Ω for 0.333. Those are the values the
    // SCHEMATICS specify; neither says the parts are populated in any given
    // assembly build, which is the next paragraph's point.
    //
    // NEITHER RATIO IS A GUARANTEE. This is open hardware: a board built from
    // the published design may have those parts changed, absent or jumpered,
    // and nothing on the wire says which. So the label pads a hot source
    // unconditionally rather than quoting a figure that only holds for the two
    // designs anybody here has read.
    //
    // A 5 V sine does NOT arrive at the chip as 5 V — roughly 1.5 Vpp.
    //
    // Whether that exceeds the part's limit or merely its recommendation is a
    // number nobody here has: the 5P49V5923 datasheet's CLKIN absolute maximum
    // could not be obtained, and the one figure findable (1.2 Vpp) belongs to
    // the 5P49V6965, a different part. The wiki does say overdriving CL1 can
    // damage the VersaClock, and its authors designed the board the divider is
    // on — but reproducing a damage mechanism we cannot check is how the last
    // three drafts of this label went wrong, each in a different way (the
    // AD9866, an unnamed "rating", a direct feed to the chip).
    //
    // So: state the requirement and the pad, both of which are the wiki's own
    // and both of which are actionable, and let "mind the level" carry the
    // caution. §4 asks for the attenuator guidance to be in the UI; it is.
    //
    // Nothing on this jack reaches the AD9866. That is the antenna path, a
    // different hazard with a different figure (>=30 dB), already warned about
    // on the Calibration page's reference combo. Do not merge the two again.
    auto* cl1Warn = new QLabel(
        "⚠ CL1's input divider is scaled for a 3.3 V logic-level clock. A "
        "hotter source — a typical GPSDO's 5 V output — wants at least 6 dB "
        "of 50 Ω attenuation in line.");
    cl1Warn->setObjectName(QStringLiteral("hl2HwCl1Warning"));
    cl1Warn->setWordWrap(true);
    cl1Warn->setAccessibleName(QStringLiteral("CL1 input level warning"));
    setTone(cl1Warn, Tone::Warn);
    mvb->addWidget(cl1Warn);

    auto* atuChk = new QCheckBox("Antenna tuner driven by the HL2 gateware");
    atuChk->setObjectName(QStringLiteral("hl2HwAtu"));
    atuChk->setToolTip(QStringLiteral(
        "Raises the gateware's tune request while TUNE is running, for an ATU wired\n"
        "to the AH-4 protocol pins.\n\n"
        "Leave this off for a tuner driven over I2C from the N2ADR IO board — with\n"
        "both enabled, two tuners are asked to start at once. Expect a short delay\n"
        "before the carrier appears while the tuner is detected."));
    mvb->addWidget(atuChk);
    controls->append(atuChk);
    vbox->addWidget(miscGroup);

    // One read path (`reseed`) and one write path (`apply`), so shown and
    // stored cannot disagree. Connections are made after the first reseed, or
    // filling the widgets would persist what was just read. QPointer guard
    // because this runs from showEvent() and connectionStateChanged.
    const QPointer<QComboBox> codecGuard(codecCombo);
    m_hl2HardwareReseed = [this, codecGuard, codecCombo, ditherChk, randomChk,
                           filterCombo, hpfChk, cl1Chk, atuChk, spkSlider,
                           noRadioLbl, controls, refreshDither, refreshHpf,
                           refreshSpeaker] {
        if (!codecGuard)
            return;
        // Through the backend's `hw.get`, not Hl2HardwareOptions: including that
        // vendor header above the radio seam is blocked by EB3
        // (tools/check_engine_boundary.py, aetherd RFC §5.5). hw.get also
        // reflects live backend state before it is persisted.
        IRadioBackend* backend = m_model->backend();
        const bool haveRadio = !m_model->settingsScope().radioId().isEmpty();
        const bool canAsk = backend
            && m_model->backendDeclaresExtension(QStringLiteral("hl2"));
        if (!canAsk) {
            // Nothing to read from and nothing that could persist a write.
            // Dim everything and say why, rather than leaving the last radio's
            // values on screen looking live.
            for (const QPointer<QWidget>& w : *controls) {
                if (w)
                    w->setEnabled(false);
            }
            if (noRadioLbl)
                noRadioLbl->setVisible(true);
            return;
        }

        // REQUEST IDS ARE NOT ALLOCATED CENTRALLY — see BandscopeDialog's note.
        // This one lives in its own decade so it cannot collide with
        // AutomationServer's counter or RadioModel's reserved UINT64_MAX.
        constexpr quint64 kHwRequestIdBase = 0x0300000000000000ull;
        static quint64 nextHwId = kHwRequestIdBase;
        const quint64 requestId = nextHwId++;
        auto* guard = new QObject(this);
        auto fill = [=, this](const QVariantMap& m) {
            const QSignalBlocker b1(codecCombo);
            const QSignalBlocker b2(ditherChk);
            const QSignalBlocker b3(randomChk);
            const QSignalBlocker b4(filterCombo);
            const QSignalBlocker b5(hpfChk);
            const QSignalBlocker b6(cl1Chk);
            const QSignalBlocker b7(atuChk);
            codecCombo->setCurrentIndex(
                codecCombo->findData(m.value(QStringLiteral("codec")).toInt()));
            ditherChk->setChecked(m.value(QStringLiteral("ditherBit")).toBool());
            randomChk->setChecked(m.value(QStringLiteral("randomBit")).toBool());
            filterCombo->setCurrentIndex(
                filterCombo->findData(m.value(QStringLiteral("filterBoard")).toInt()));
            hpfChk->setChecked(m.value(QStringLiteral("n2adrHpf")).toBool());
            cl1Chk->setChecked(m.value(QStringLiteral("cl1RefClock")).toBool());
            // THE CALIBRATION PAGE HAS TO LEARN THIS, because §4 requires the
            // manual ppb control to be DISABLED under a locked reference and
            // that control lives on another page. Cached rather than fetched
            // there: the Calibration page would otherwise need its own hw.get
            // round trip, and it reads its value straight out of the settings
            // scope. Refreshed below once this reply has landed, which is what
            // makes the ordering safe — showEvent() runs both reseeds and this
            // one is asynchronous, so the calibration page can render before
            // the answer arrives and must be corrected when it does.
            m_hl2ExternalRefLocked = m.value(QStringLiteral("cl1RefClock")).toBool();
            atuChk->setChecked(m.value(QStringLiteral("atuGateware")).toBool());
            // Clamped here too, not only in the backend: this arrives as a
            // QVariant off a generic seam, and a slider given a value outside
            // its range silently takes the nearest end — which would then be
            // persisted by the next drag as if the operator had chosen it.
            spkSlider->setValue(std::clamp(
                m.value(QStringLiteral("speakerLevelPercent"), 100).toInt(), 0, 100));
            // Inside the blockers with everything else, though these three
            // only relabel and dim now — nothing here writes a control's value.
            refreshDither();
            refreshHpf();
            refreshSpeaker();
            if (m_calibrationReseed) {
                m_calibrationReseed();
            }
            // No radio identity, no write (the backend refuses to write the
            // family-wide row), so disable the controls. One-way: the refresh
            // calls above already set each control's availability; this only
            // takes it away.
            if (!haveRadio) {
                for (const QPointer<QWidget>& w : *controls) {
                    if (w)
                        w->setEnabled(false);
                }
            }
            if (noRadioLbl)
                noRadioLbl->setVisible(!haveRadio);
        };

        connect(backend, &IRadioBackend::extensionResult, guard,
                [guard, requestId, fill](quint64 id, const QVariant& result) {
            if (id != requestId)
                return;
            guard->deleteLater();
            fill(result.toMap());
        });
        connect(backend, &IRadioBackend::extensionError, guard,
                [guard, requestId](quint64 id, const QString&) {
            if (id != requestId)
                return;
            guard->deleteLater();   // leave the page as it stands
        });
        // Connected BEFORE the invoke: hw.get completes locally in the backend
        // and emits its reply SYNCHRONOUSLY, so a handler armed afterwards
        // would never see it.
        m_model->invokeBackendExtension(QStringLiteral("hl2"),
                                        QStringLiteral("hw.get"), requestId);
    };
    m_hl2HardwareReseed();

    connect(codecCombo, &QComboBox::currentIndexChanged, this,
            [this, apply, codecCombo, refreshDither, refreshSpeaker](int) {
        const int codec = codecCombo->currentData().toInt();
        refreshDither();
        refreshSpeaker();
        // Send the codec ALONE: bit 0x00[11] means different things per board,
        // and hw.set applies Hl2HardwareOptions::ditherBitOnCodecChange() when
        // the codec moves without an explicit bit, for bridge callers too. The
        // reseed then shows what the backend decided. Calling the policy here
        // would need the vendor header above the seam (EB3).
        apply(QVariantMap{{QStringLiteral("codec"), codec}});
        if (m_hl2HardwareReseed)
            m_hl2HardwareReseed();
    });
    connect(ditherChk, &QCheckBox::toggled, this, [apply](bool on) {
        apply(QVariantMap{{QStringLiteral("ditherBit"), on}});
    });
    connect(randomChk, &QCheckBox::toggled, this, [apply](bool on) {
        apply(QVariantMap{{QStringLiteral("randomBit"), on}});
    });
    connect(filterCombo, &QComboBox::currentIndexChanged, this,
            [apply, filterCombo, refreshHpf](int) {
        refreshHpf();
        apply(QVariantMap{
            {QStringLiteral("filterBoard"), filterCombo->currentData().toInt()}});
    });
    connect(hpfChk, &QCheckBox::toggled, this, [apply](bool on) {
        apply(QVariantMap{{QStringLiteral("n2adrHpf"), on}});
    });
    connect(cl1Chk, &QCheckBox::toggled, this, [this, apply](bool on) {
        apply(QVariantMap{{QStringLiteral("cl1RefClock"), on}});
        // Immediately, not on the next reseed. The backend zeroes the manual
        // ppb as part of this write, so the Calibration page is stale the
        // instant the box is ticked — and a spin box still showing -178 ppb
        // next to a disabled control is worse than either state alone.
        m_hl2ExternalRefLocked = on;
        if (m_calibrationReseed) {
            m_calibrationReseed();
        }
    });
    connect(atuChk, &QCheckBox::toggled, this, [apply](bool on) {
        apply(QVariantMap{{QStringLiteral("atuGateware"), on}});
    });
    // The READOUT follows every drag; the WRITE waits for the release. Dragging
    // a slider emits valueChanged on every pixel, and persisting each one would
    // put a full non-atomic settings write behind every one of them — the same
    // reason the Calibration page's Trim buttons commit on release.
    connect(spkSlider, &QSlider::valueChanged, this, [spkValue](int v) {
        spkValue->setText(QStringLiteral("%1%").arg(v));
    });
    connect(spkSlider, &QSlider::sliderReleased, this, [apply, spkSlider] {
        apply(QVariantMap{{QStringLiteral("speakerLevelPercent"), spkSlider->value()}});
    });
    // Keyboard and mouse-wheel changes never emit sliderReleased, so they would
    // move the readout and persist nothing. actionTriggered fires for exactly
    // those, and not for a drag — sliderMoved handles the drag and is already
    // committed on release above.
    connect(spkSlider, &QSlider::actionTriggered, this, [apply, spkSlider](int action) {
        if (action == QAbstractSlider::SliderMove)
            return;                       // the drag; committed on release
        // Queued: actionTriggered fires BEFORE the value moves, so reading it
        // here would persist the value the slider is leaving.
        QMetaObject::invokeMethod(spkSlider, [apply, spkSlider] {
            apply(QVariantMap{{QStringLiteral("speakerLevelPercent"), spkSlider->value()}});
        }, Qt::QueuedConnection);
    });

    vbox->addStretch(1);
    return page;
}

// ── Droop Correction tab ────────────────────────────────────────────────────

QWidget* RadioSetupDialog::buildDroopCalibrationTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);



    auto* group = new QGroupBox("DDC0 Droop Correction");
    auto* gvb = new QVBoxLayout(group);
    gvb->setSpacing(8);

    {
        auto* intro = new QLabel(
            "This radio's DDC has a real amplitude droop near the edges of the "
            "displayed span. A correction derived from the radio's own gateware "
            "ships with AetherSDR and is applied automatically; sweeping here "
            "measures YOUR radio and replaces it. For the most "
            "accurate correction, disconnect the antenna or terminate it in a "
            "dummy load before starting — the sweep measures the receiver's own "
            "noise floor as a flat reference, and a live signal during the "
            "sweep will bias the correction for whichever rate it lands in.");
        applyLabelStyle(intro);
        intro->setWordWrap(true);
        gvb->addWidget(intro);
    }

    auto* noRadioLbl = new QLabel(
        "Connect the radio first. The calibration belongs to one physical "
        "radio, and there is no radio identity to store it against yet.");
    setTone(noRadioLbl, Tone::Error);
    noRadioLbl->setWordWrap(true);
    noRadioLbl->setVisible(false);
    gvb->addWidget(noRadioLbl);

    auto* startStopBtn = new QPushButton("Start Sweep");
    startStopBtn->setFixedWidth(120);
    // Description, NOT accessibleName: this button's text toggles between
    // "Start Sweep" and "Stop", and a screen reader takes a button's name
    // from its text unless one is set explicitly. A fixed name here would
    // freeze the announcement at "Start sweep" while the button actually
    // reads "Stop" -- worse than saying nothing. The description supplements
    // the live text instead of replacing it.
    startStopBtn->setAccessibleDescription(QStringLiteral(
        "Steps the radio through every DDC0 sample rate and measures the "
        "panadapter's edge droop at each one. Takes several minutes."));

    auto* progressBar = new QProgressBar;
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setTextVisible(false);
    progressBar->setFixedHeight(startStopBtn->sizeHint().height());
    // setTextVisible(false) leaves this with no text at all, so without a
    // name it is announced as an unlabelled progress bar.
    progressBar->setAccessibleName(QStringLiteral("Droop sweep progress"));

    auto* rowLayout = new QHBoxLayout;
    rowLayout->addWidget(startStopBtn);
    rowLayout->addWidget(progressBar, 1);
    gvb->addLayout(rowLayout);

    auto* statusLbl = new QLabel("Idle — no sweep has been run this session.");
    applyLabelStyle(statusLbl);
    statusLbl->setWordWrap(true);
    // Same reasoning as the Start button, for the same reason in reverse: a
    // QLabel's accessible name IS its text, and this label's text is the
    // live sweep status (including the failure reasons the calibrator
    // reports). Naming it would hide exactly the content worth hearing.
    statusLbl->setAccessibleDescription(QStringLiteral("Droop sweep status"));
    gvb->addWidget(statusLbl);

    auto* summaryLbl = new QLabel;
    setTone(summaryLbl, Tone::Hint);
    summaryLbl->setWordWrap(true);
    summaryLbl->setAccessibleDescription(QStringLiteral(
        "Measured correction per DDC0 rate"));
    gvb->addWidget(summaryLbl);

    auto* applyBtn = new QPushButton("Apply");
    applyBtn->setFixedWidth(90);
    applyBtn->setEnabled(false);
    // Static text, so a name is safe here -- and needed: "Apply" and
    // "Discard" alone say nothing about what is being applied or discarded.
    applyBtn->setAccessibleName(QStringLiteral("Apply the measured droop correction"));
    applyBtn->setToolTip(QStringLiteral(
        "Push the measured tables live and save them for this radio"));

    auto* cancelBtn = new QPushButton("Discard");
    cancelBtn->setFixedWidth(90);
    cancelBtn->setEnabled(false);
    cancelBtn->setAccessibleName(QStringLiteral("Discard the measured droop correction"));
    cancelBtn->setToolTip(QStringLiteral("Drop the measured (not yet applied) result"));

    auto* applyRow = new QHBoxLayout;
    applyRow->addWidget(applyBtn);
    applyRow->addWidget(cancelBtn);
    applyRow->addStretch(1);
    gvb->addLayout(applyRow);

    vbox->addWidget(group);

    auto update = [this, startStopBtn, progressBar, statusLbl, summaryLbl,
                   applyBtn, cancelBtn, noRadioLbl](const QVariantMap& state) {
        const bool available = droopCalibrationAvailable(m_model->backend());
        const bool running = available && state.value(QStringLiteral("running")).toBool();
        const bool hasResult = available && state.value(QStringLiteral("hasResult")).toBool();
        startStopBtn->setEnabled(available);
        startStopBtn->setText(running ? QStringLiteral("Stop") : QStringLiteral("Start Sweep"));
        startStopBtn->setProperty("droopRunning", running);
        applyBtn->setEnabled(hasResult && !running);
        cancelBtn->setEnabled(hasResult && !running);
        noRadioLbl->setVisible(!available);
        progressBar->setValue(available ? state.value(QStringLiteral("percent")).toInt() : 0);
        QString message = state.value(QStringLiteral("message")).toString();
        if (state.contains(QStringLiteral("error"))) {
            message = state.value(QStringLiteral("error")).toString();
        }
        statusLbl->setText(message.isEmpty()
            ? QStringLiteral("Idle — no sweep has been run this session.") : message);
        QStringList lines;
        if (available) {
            const QVariantList corrections = state.value(QStringLiteral("corrections")).toList();
            for (const QVariant& value : corrections) {
                const QVariantMap correction = value.toMap();
                // Tag the source per rate. A partial sweep leaves the rest on
                // the shipped default, and collapsing the two into one list
                // would hide which is which -- see AnanBackend::droopStatus().
                const bool measured =
                    correction.value(QStringLiteral("source")).toString()
                        == QLatin1String("measured");
                lines << QStringLiteral("%1 ksps: %2–%3 dB  (%4)")
                    .arg(correction.value(QStringLiteral("rateKsps")).toInt(), 4)
                    .arg(correction.value(QStringLiteral("minDb")).toDouble(), 0, 'f', 1)
                    .arg(correction.value(QStringLiteral("maxDb")).toDouble(), 0, 'f', 1)
                    .arg(measured ? QStringLiteral("measured")
                                  : QStringLiteral("shipped default"));
            }
        }
        summaryLbl->setText(lines.join(QStringLiteral("\n")));
    };
    auto request = [this, update](const QString& action) {
        update(requestDroopCalibration(m_model->backend(), action));
    };
    connect(startStopBtn, &QPushButton::clicked, this, [startStopBtn, request] {
        request(startStopBtn->property("droopRunning").toBool()
                    ? QStringLiteral("stop") : QStringLiteral("start"));
    });
    connect(applyBtn, &QPushButton::clicked, this, [request] {
        request(QStringLiteral("apply"));
    });
    connect(cancelBtn, &QPushButton::clicked, this, [request] {
        request(QStringLiteral("discard"));
    });

    // Rebind to the current neutral backend on reconnect. No captured ANAN
    // object survives a family switch; both entry points re-check capability.
    m_droopReseed = [this, update] {
        QObject::disconnect(m_droopStatusConnection);
        IRadioBackend* backend = m_model->backend();
        if (backend && backend->capabilities().family == QLatin1String("anan")
            && backend->capabilities().hostDroopCalibration) {
            m_droopStatusConnection = connect(backend, &IRadioBackend::extensionStatus, this,
                [this, backend, update](const QString& ns, const QString& kind,
                                        const QVariantMap& state) {
                    if (m_model->backend() == backend && ns == QLatin1String("anan")
                        && kind == QLatin1String("droop")) {
                        update(state);
                    }
                });
            update(droopCalibrationAvailable(backend)
                ? requestDroopCalibration(backend, QStringLiteral("status")) : QVariantMap{});
        } else {
            update({});
        }
    };
    m_droopReseed();

    vbox->addStretch(1);
    return page;
}

// ── Audio tab ────────────────────────────────────────────────────────────────

namespace {

// Refill a PC-audio device combo from a live enumeration, keyed by device ID
// (rows reorder on hot-plug). Signals stay blocked: currentIndexChanged rebuilds
// the QAudioSource (#1114), and a device arriving must not re-point the engine
// (#2864); vanishing devices are handleAudioDeviceListChanged's job. The combo
// displays the engine's device, falling back to its own selection.
void repopulateAudioDeviceCombo(QComboBox* combo,
                                const QList<QAudioDevice>& devices,
                                const QAudioDevice& engineDevice)
{
    if (!combo)
        return;
    // Engine first. The combo's currentData() is better only between a click
    // and setInput/OutputDevice landing on the audio thread (#1114); otherwise
    // a reseed after applyAudioDeviceSelection (both setters emit) would keep
    // showing the old device while audio runs elsewhere.
    const QByteArray previousId = combo->currentData().toByteArray();
    QByteArray wantedId = engineDevice.id();
    if (wantedId.isEmpty())
        wantedId = previousId;

    QSignalBlocker blocker(combo);
    combo->clear();
    for (const auto& dev : devices)
        combo->addItem(dev.description(), dev.id());

    int idx = combo->findData(wantedId);
    if (idx < 0)
        idx = combo->findData(previousId);
    if (idx >= 0)
        combo->setCurrentIndex(idx);
}

} // namespace

QWidget* RadioSetupDialog::buildAudioTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);

    // ── Radio Audio Outputs ──────────────────────────────────────────────
    auto* outGroup = new QGroupBox("Radio Audio Outputs");
    auto* outLayout = new QVBoxLayout(outGroup);

    // Line Out
    auto* lineoutRow = new QHBoxLayout;
    auto* lineoutLabel = new QLabel("Line Out:");
    applyLabelStyle(lineoutLabel);
    lineoutLabel->setFixedWidth(90);
    auto* lineoutSlider = new GuardedSlider(Qt::Horizontal);
    lineoutSlider->setRange(0, 100);
    lineoutSlider->setValue(m_model->lineoutGain());
    auto* lineoutValue = makeValueLabel(QString::number(m_model->lineoutGain()));
    lineoutValue->setFixedWidth(30);
    auto* lineoutMute = new QPushButton("Mute");
    lineoutMute->setCheckable(true);
    lineoutMute->setChecked(m_model->lineoutMute());
    lineoutMute->setFixedWidth(50);
    setButtonRole(lineoutMute, ButtonRole::Mute);
    lineoutRow->addWidget(lineoutLabel);
    lineoutRow->addWidget(lineoutSlider, 1);
    lineoutRow->addWidget(lineoutValue);
    lineoutRow->addWidget(lineoutMute);
    outLayout->addLayout(lineoutRow);

    connect(lineoutSlider, &QSlider::valueChanged, this, [this, lineoutValue](int v) {
        lineoutValue->setText(QString::number(v));
        m_model->setLineoutGain(v);
    });
    connect(lineoutMute, &QPushButton::toggled, m_model, &RadioModel::setLineoutMute);
    // Dimmed, never hidden, on a radio with no line out of its own; re-applied
    // live because this dialog can stay open across a reconnect.
    auto applyLineoutAvailability = [this, lineoutLabel, lineoutSlider,
                                     lineoutValue, lineoutMute] {
        const bool available = AetherSDR::lineoutControlsAvailable(
            m_model->isConnected(), m_model->hasCommandPlane(),
            m_model->backendCapabilities().lineoutControl.has_value());
        const QString reason = available
            ? QString()
            : QStringLiteral("Unavailable: this radio has no line out "
                             "AetherSDR can control.");
        for (QWidget* w : {static_cast<QWidget*>(lineoutLabel),
                           static_cast<QWidget*>(lineoutSlider),
                           static_cast<QWidget*>(lineoutValue),
                           static_cast<QWidget*>(lineoutMute)}) {
            w->setEnabled(available);
            w->setToolTip(reason);
            w->setAccessibleDescription(reason);
        }
    };
    applyLineoutAvailability();
    connect(m_model, &RadioModel::connectionStateChanged, lineoutMute,
            applyLineoutAvailability);
    connect(m_model, &RadioModel::capabilitiesChanged, lineoutMute,
            applyLineoutAvailability);

    // Headphone
    auto* hpRow = new QHBoxLayout;
    auto* hpLabel = new QLabel("Headphone:");
    applyLabelStyle(hpLabel);
    hpLabel->setFixedWidth(90);
    auto* hpSlider = new GuardedSlider(Qt::Horizontal);
    hpSlider->setRange(0, 100);
    hpSlider->setValue(m_model->headphoneGain());
    auto* hpValue = makeValueLabel(QString::number(m_model->headphoneGain()));
    hpValue->setFixedWidth(30);
    auto* hpMute = new QPushButton("Mute");
    hpMute->setCheckable(true);
    hpMute->setChecked(m_model->headphoneMute());
    hpMute->setFixedWidth(50);
    setButtonRole(hpMute, ButtonRole::Mute);
    hpRow->addWidget(hpLabel);
    hpRow->addWidget(hpSlider, 1);
    hpRow->addWidget(hpValue);
    hpRow->addWidget(hpMute);
    outLayout->addLayout(hpRow);

    connect(hpSlider, &QSlider::valueChanged, this, [this, hpValue](int v) {
        hpValue->setText(QString::number(v));
        m_model->setHeadphoneGain(v);
    });
    connect(hpMute, &QPushButton::toggled, m_model, &RadioModel::setHeadphoneMute);

    // Dimmed, never hidden, on a radio with no headphone output: the title
    // bar's predicate and its two reasons (MixerControlAvailability.h), one
    // for the level and one for the mute.
    auto applyHeadphoneAvailability = [this, hpLabel, hpSlider, hpValue, hpMute] {
        const bool available = AetherSDR::headphoneControlsAvailable(
            m_model->isConnected(), m_model->hasCommandPlane());
        const QString levelReason = available
            ? QString()
            : QString::fromLatin1(AetherSDR::kHeadphoneLevelUnavailableReason);
        const QString muteReason = available
            ? QString()
            : QString::fromLatin1(AetherSDR::kHeadphoneMuteUnavailableReason);
        for (QWidget* w : {static_cast<QWidget*>(hpLabel),
                           static_cast<QWidget*>(hpSlider),
                           static_cast<QWidget*>(hpValue)}) {
            w->setEnabled(available);
            w->setToolTip(levelReason);
            w->setAccessibleDescription(levelReason);
        }
        hpMute->setEnabled(available);
        hpMute->setToolTip(muteReason);
        hpMute->setAccessibleDescription(muteReason);
    };
    applyHeadphoneAvailability();
    connect(m_model, &RadioModel::connectionStateChanged, hpMute,
            applyHeadphoneAvailability);

    // Front Speaker (mute only) — only on M-suffix models with built-in speaker
    // M-suffix models have a built-in front speaker (6400M, 6600M, 8400M, 8600M, AU-510M, AU-520M)
    bool hasFrontSpeaker = m_model->model().endsWith("M", Qt::CaseInsensitive);
    if (hasFrontSpeaker) {
        auto* spkRow = new QHBoxLayout;
        auto* spkLabel = new QLabel("Front Speaker:");
        applyLabelStyle(spkLabel);
        spkLabel->setFixedWidth(90);
        auto* spkMute = new QPushButton("Mute");
        spkMute->setCheckable(true);
        spkMute->setChecked(m_model->frontSpeakerMute());
        spkMute->setFixedWidth(50);
        setButtonRole(spkMute, ButtonRole::Mute);
        spkRow->addWidget(spkLabel);
        spkRow->addStretch(1);
        spkRow->addWidget(spkMute);
        outLayout->addLayout(spkRow);
        connect(spkMute, &QPushButton::toggled, m_model, &RadioModel::setFrontSpeakerMute);
    }

    // Update from radio status
    connect(m_model, &RadioModel::audioOutputChanged, this,
            [this, lineoutSlider, lineoutValue, lineoutMute,
             hpSlider, hpValue, hpMute] {
        QSignalBlocker b1(lineoutSlider), b2(lineoutMute),
                       b3(hpSlider), b4(hpMute);
        lineoutSlider->setValue(m_model->lineoutGain());
        lineoutValue->setText(QString::number(m_model->lineoutGain()));
        lineoutMute->setChecked(m_model->lineoutMute());
        hpSlider->setValue(m_model->headphoneGain());
        hpValue->setText(QString::number(m_model->headphoneGain()));
        hpMute->setChecked(m_model->headphoneMute());
    });

    vbox->addWidget(outGroup);

    // ── Audio Compression ────────────────────────────────────────────────
    {
        auto* compGroup = new QGroupBox("Audio Compression (SmartLink / tailnet)");
        compGroup->setToolTip(QStringLiteral(
            "Auto uses Opus for SmartLink and for a radio reached over a tailnet, and "
            "uncompressed audio on the local network. Until you choose, a radio reached "
            "over a tailnet gets Opus."));
        m_audioCompressionGroup = compGroup;
        compGroup->setVisible(!m_model->isConnected()
                              || m_model->backendCapabilities().hasAudioCompression);
        auto* compLayout = new QHBoxLayout(compGroup);
        compLayout->setSpacing(4);

        // Never chosen: show what applies to this connection (Opus over a
        // tailnet, uncompressed otherwise; RFC #6271 D4) without saving it.
        QString current = AppSettings::instance().value("AudioCompression", "None").toString();
        if (!AppSettings::instance().contains(QStringLiteral("AudioCompression"))) {
            current = (m_model->isConnected() && m_model->audioCompressionParam() == "opus")
                ? QStringLiteral("Opus") : QStringLiteral("None");
        }


        auto* autoBtn = new QPushButton("Auto");
        autoBtn->setCheckable(true); autoBtn->setChecked(current == "Auto");
        setButtonRole(autoBtn, ButtonRole::Select);
        auto* noneBtn = new QPushButton("Uncompressed");
        noneBtn->setCheckable(true); noneBtn->setChecked(current == "None");
        setButtonRole(noneBtn, ButtonRole::Select);
        auto* opusBtn = new QPushButton("Opus");
        opusBtn->setCheckable(true); opusBtn->setChecked(current == "Opus");
        setButtonRole(opusBtn, ButtonRole::Select);

        auto setComp = [autoBtn, noneBtn, opusBtn](const QString& val) {
            QSignalBlocker b1(autoBtn), b2(noneBtn), b3(opusBtn);
            autoBtn->setChecked(val == "Auto");
            noneBtn->setChecked(val == "None");
            opusBtn->setChecked(val == "Opus");
            auto& s = AppSettings::instance();
            s.setValue("AudioCompression", val);
            s.save();
        };

        connect(autoBtn, &QPushButton::clicked, this, [setComp]() { setComp("Auto"); });
        connect(noneBtn, &QPushButton::clicked, this, [setComp]() { setComp("None"); });
        connect(opusBtn, &QPushButton::clicked, this, [setComp]() { setComp("Opus"); });

        compLayout->addWidget(autoBtn);
        compLayout->addWidget(noneBtn);
        compLayout->addWidget(opusBtn);
        compLayout->addStretch();

        auto* hint = new QLabel("Auto = Opus on SmartLink or a tailnet, uncompressed on LAN");
        setTone(hint, Tone::Hint);
        compLayout->addWidget(hint);

        vbox->addWidget(compGroup);
    }

    // The two connection-wide switches share a group rather than sitting
    // loose between groups.
    auto* connectionGroup = new QGroupBox("Connection");
    auto* connectionBox = new QVBoxLayout(connectionGroup);
    connectionBox->setSpacing(6);
    vbox->addWidget(connectionGroup);

    // ── Packet-Loss Concealment ─────────────────────────────────────────
    // Fades dropped VITA-49 audio packets to silence (uncompressed) or
    // calls libopus native PLC (Opus) instead of splicing the next packet
    // directly. Cuts the broadband click on lossy WAN/SmartLink. (#2731)
    {
        auto* plcCheck = new QCheckBox(
            "Smooth packet loss (conceal dropped audio packets)");
        plcCheck->setToolTip(
            "When the radio's audio stream loses a UDP packet, fade the gap\n"
            "to silence (uncompressed) or synthesize a perceptually smooth\n"
            "fill with libopus PLC (Opus) instead of splicing the next packet\n"
            "directly. Reduces the high-pitch click that the splice produces\n"
            "on lossy WAN/SmartLink links. Capped at ~80 ms before the audio\n"
            "drops to clean silence.");
        plcCheck->setChecked(
            AppSettings::instance()
                .value("AudioPacketLossConcealment", "True").toString() == "True");
        connect(plcCheck, &QCheckBox::toggled, this, [this](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("AudioPacketLossConcealment", on ? "True" : "False");
            s.save();
            // PanadapterStream lives on the network worker thread (#502);
            // route the toggle through QueuedConnection so the atomic and
            // map mutations happen on the owning thread.
            if (m_model && m_model->panStream()) {
                QMetaObject::invokeMethod(
                    m_model->panStream(),
                    [stream = m_model->panStream(), on]() {
                        stream->setPacketLossConcealment(on);
                    },
                    Qt::QueuedConnection);
            }
        });
        connectionBox->addWidget(plcCheck);
    }

    // ── Prevent Sleep ───────────────────────────────────────────────────
    // On by default (#6192). Idle sleep is the only thing this governs:
    // keepAppActive() stops App Nap / power throttling for the app's
    // lifetime but never blocks sleep.
    {
        auto* sleepCheck = new QCheckBox("Prevent system sleep while connected");
        sleepCheck->setToolTip("Hold a system power assertion to prevent idle sleep\n"
                               "while connected to a radio. Keeps TCP/UDP/audio\n"
                               "streams alive during long sessions.");
        sleepCheck->setChecked(RadioModel::sleepInhibitWhileConnected());
        connect(sleepCheck, &QCheckBox::toggled, this, [this](bool on) {
            m_model->setSleepInhibitWhileConnected(on);
        });
        connectionBox->addWidget(sleepCheck);
    }

    // ── PC Audio Devices ────────────────────────────────────────────────
    auto* pcGroup = new QGroupBox("PC Audio Devices");
    auto* pcLayout = new QVBoxLayout(pcGroup);

    // Input device
    auto* inRow = new QHBoxLayout;
    auto* inLabel = new QLabel("Input:");
    applyLabelStyle(inLabel);
    inLabel->setFixedWidth(90);
    auto* inCombo = new QComboBox;
    AetherSDR::applyComboStyle(inCombo);
    // Filled by reseedAudioDeviceCombos below, not from a snapshot taken here.
    inRow->addWidget(inLabel);
    inRow->addWidget(inCombo, 1);
    pcLayout->addLayout(inRow);

    // Output device
    auto* outRow = new QHBoxLayout;
    auto* outLabel = new QLabel("Output:");
    applyLabelStyle(outLabel);
    outLabel->setFixedWidth(90);
    auto* outCombo = new QComboBox;
    AetherSDR::applyComboStyle(outCombo);
    // Filled by reseedAudioDeviceCombos below, not from a snapshot taken here.
    outRow->addWidget(outLabel);
    outRow->addWidget(outCombo, 1);
    pcLayout->addLayout(outRow);

    auto* promptCheck = new QCheckBox("Prompt on Audio Device Changes");
    promptCheck->setToolTip("Show the Audio Device Detected dialog when a new PC audio device appears.");
    const bool suppressAudioDeviceNotifications =
        AppSettings::instance()
            .value(kSuppressAudioDeviceNotificationsKey, "False")
            .toString() == "True";
    promptCheck->setChecked(!suppressAudioDeviceNotifications);
    connect(promptCheck, &QCheckBox::toggled, this, [](bool on) {
        auto& s = AppSettings::instance();
        s.setValue(kSuppressAudioDeviceNotificationsKey, on ? "False" : "True");
        s.save();
    });
    pcLayout->addWidget(promptCheck);

    // ── Keep both combos level with the platform ──────────────────────
    // This page is built ONCE per process (buildDeferredTab erases the builder,
    // #1776) and the dialog is a showOrRaisePersistent singleton that is hidden
    // rather than destroyed. A list enumerated at build time is therefore the
    // list the user sees for the rest of the session: connect a headset with
    // this pane open and it never appears — and closing and reopening Settings
    // does not bring it back either, because nothing is rebuilt. Watch the
    // platform instead of trusting one snapshot.
    auto reseedAudioDeviceCombos = [this, inCombo, outCombo] {
        const QAudioDevice curIn = m_audio ? m_audio->inputDevice() : QAudioDevice();
        const QAudioDevice curOut = m_audio ? m_audio->outputDevice() : QAudioDevice();
        repopulateAudioDeviceCombo(inCombo, QMediaDevices::audioInputs(),
            curIn.isNull() ? QMediaDevices::defaultAudioInput() : curIn);
        repopulateAudioDeviceCombo(outCombo, QMediaDevices::audioOutputs(),
            curOut.isNull() ? QMediaDevices::defaultAudioOutput() : curOut);
    };
    reseedAudioDeviceCombos();

    std::function<void()> refreshAudioDeviceCombos;
    {
        // One QMediaDevices per built page, parented to the group box. That it
        // is constructed HERE and not in the dialog constructor is the whole
        // point of #1776: hardware probing happens when the user opens the
        // Audio page, never before.
        auto* audioDeviceMonitor = new QMediaDevices(pcGroup);
        auto* settle = new QTimer(pcGroup);
        settle->setSingleShot(true);
        // Deliberately trails MainWindow::setupAudioDeviceChangeMonitor's own
        // 750 ms settle: when a removal forces the engine back to the default
        // we want to show the device it ended up on, not the one that vanished.
        settle->setInterval(900);
        refreshAudioDeviceCombos = [settle, inCombo, outCombo, reseedAudioDeviceCombos] {
            // Never pull the list out from under an open dropdown. PipeWire can
            // churn device IDs continuously (#2864); a combo that rebuilds
            // while the user is reading it is worse than one that waits.
            const bool popupOpen =
                inCombo->view()->isVisible() || outCombo->view()->isVisible();
            if (popupOpen) {
                settle->start();
                return;
            }
            reseedAudioDeviceCombos();
        };
        connect(settle, &QTimer::timeout, this, refreshAudioDeviceCombos);
        connect(audioDeviceMonitor, &QMediaDevices::audioInputsChanged,
                settle, qOverload<>(&QTimer::start));
        connect(audioDeviceMonitor, &QMediaDevices::audioOutputsChanged,
                settle, qOverload<>(&QTimer::start));
    }
    if (m_audio) {
        // The SELECTION can go stale without the list changing at all: the
        // hot-plug prompt and resetMissingAudioDevicesToDefault both re-point
        // the engine behind this pane's back. Queued across the audio thread.
        connect(m_audio, &AudioEngine::inputDeviceChanged, this,
                refreshAudioDeviceCombos);
        connect(m_audio, &AudioEngine::outputDeviceChanged, this,
                refreshAudioDeviceCombos);
    }
    // Same contract as m_calibrationReseed: re-read on every show, so a change
    // that landed while the dialog was hidden cannot survive a close and reopen.
    m_audioDeviceReseed = refreshAudioDeviceCombos;

    // Wire device changes to AudioEngine
    if (m_audio) {
        // Route through QueuedConnection so setInputDevice/setOutputDevice
        // execute on the audio worker thread, preventing use-after-free on
        // macOS CoreAudio when switching devices from the GUI thread (#1114).
        // Resolve the choice by device ID against a LIVE enumeration. These
        // lambdas used to capture the build-time device list by value and index
        // into it, which is only correct while the combo can never be refilled.
        auto applyChoice = [this](const QByteArray& id, bool input) {
            const QList<QAudioDevice> devices =
                input ? QMediaDevices::audioInputs() : QMediaDevices::audioOutputs();
            for (const QAudioDevice& dev : devices) {
                if (dev.id() != id)
                    continue;
                // Still queued onto the audio thread: switching a device from
                // the GUI thread is a use-after-free on macOS CoreAudio (#1114).
                QMetaObject::invokeMethod(m_audio, [this, dev, input]() {
                    if (input)
                        m_audio->setInputDevice(dev);
                    else
                        m_audio->setOutputDevice(dev);
                }, Qt::QueuedConnection);
                return;
            }
            // The device vanished between the reseed that offered it and the
            // click that chose it. Nothing to do but say so: without this the
            // symptom is "I picked it and nothing happened", with no trace.
            qCWarning(lcAudio)
                << "RadioSetupDialog: chosen" << (input ? "input" : "output")
                << "device" << id << "is no longer enumerable; selection dropped";
        };
        connect(inCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [inCombo, applyChoice](int idx) {
            if (idx >= 0)
                applyChoice(inCombo->itemData(idx).toByteArray(), true);
        });
        connect(outCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [outCombo, applyChoice](int idx) {
            if (idx >= 0)
                applyChoice(outCombo->itemData(idx).toByteArray(), false);
        });
    }

    // Audio Boost toggle
    {
        auto* boostRow = new QHBoxLayout;
        auto* boostLabel = new QLabel("Audio Boost:");
        applyLabelStyle(boostLabel);
        boostLabel->setFixedWidth(90);
        bool boostOn = AppSettings::instance().value("AudioBoost", "False").toString() == "True";
        auto* boostBtn = new QPushButton(boostOn ? "Enabled" : "Disabled");
        boostBtn->setCheckable(true);
        boostBtn->setChecked(boostOn);
        boostBtn->setToolTip("Apply 50% software gain boost to PC audio output.\n"
                             "Compensates for lower levels with AGC-controlled audio.");
        connect(boostBtn, &QPushButton::toggled, this, [this, boostBtn](bool on) {
            boostBtn->setText(on ? "Enabled" : "Disabled");
            auto& s = AppSettings::instance();
            s.setValue("AudioBoost", on ? "True" : "False");
            s.save();
            if (m_audio) {
                QMetaObject::invokeMethod(m_audio, [this, on]() {
                    m_audio->setRxBoost(on);
                }, Qt::QueuedConnection);
            }
        });
        boostRow->addWidget(boostLabel);
        boostRow->addWidget(boostBtn);
        boostRow->addStretch(1);
        pcLayout->addLayout(boostRow);
    }

    // Audio Buffer (ms)
    {
        auto* bufRow = new QHBoxLayout;
        auto* bufLabel = new QLabel("Audio Buffer:");
        applyLabelStyle(bufLabel);
        bufLabel->setFixedWidth(90);
        int bufMs = AppSettings::instance().value("AudioBufferMs", "100").toInt();
        auto* bufEdit = new QLineEdit(QString::number(bufMs));
        bufEdit->setFixedWidth(50);
        auto* bufUnit = new QLabel("ms");
        applyLabelStyle(bufUnit);
        auto* bufHint = new QLabel("(50–1000, increase for VPN/SmartLink jitter)");
        setTone(bufHint, Tone::Hint);
        connect(bufEdit, &QLineEdit::editingFinished, this, [this, bufEdit] {
            int val = qBound(50, bufEdit->text().toInt(), 1000);
            bufEdit->setText(QString::number(val));
            auto& s = AppSettings::instance();
            s.setValue("AudioBufferMs", QString::number(val));
            s.save();
            if (m_audio) {
                QMetaObject::invokeMethod(m_audio, [this, val]() {
                    m_audio->setRxBufferCapMs(val);
                }, Qt::QueuedConnection);
            }
        });
        bufRow->addWidget(bufLabel);
        bufRow->addWidget(bufEdit);
        bufRow->addWidget(bufUnit);
        bufRow->addWidget(bufHint);
        bufRow->addStretch(1);
        pcLayout->addLayout(bufRow);
    }

    vbox->addWidget(pcGroup);

    // ── Recording ───────────────────────────────────────────────────────
    {
        auto* recGroup = new QGroupBox("Recording");
        auto* recLayout = new QVBoxLayout(recGroup);

        auto& settings = AppSettings::instance();

        // Mode: Radio Side vs Client Side
        auto* modeRow = new QHBoxLayout;
        auto* modeLabel = new QLabel("Record Mode:");
        applyLabelStyle(modeLabel);
        modeLabel->setFixedWidth(90);
        modeRow->addWidget(modeLabel);


        auto* radioSideBtn = new QPushButton("Radio Side");
        radioSideBtn->setCheckable(true);
        setButtonRole(radioSideBtn, ButtonRole::Select);
        auto* clientSideBtn = new QPushButton("Client Side");
        clientSideBtn->setCheckable(true);
        setButtonRole(clientSideBtn, ButtonRole::Select);

        // A connected radio with no command plane has no radio-side recorder, so
        // Client Side is in effect there (recordsOnClient()). Shown, never
        // written: the saved choice returns on the next radio that has one.
        // By hand, not ControlAvailabilityRegistry: command-plane reachability
        // is not in RadioCapabilities.
        const auto applyRecordingModeAvailability = [this, radioSideBtn, clientSideBtn]() {
            const bool radioSideAvailable =
                !m_model->isConnected() || m_model->radioSideRecordingReachable();
            const bool clientSide = !radioSideAvailable
                || AppSettings::instance().value("RecordingMode", "Client").toString()
                       == "Client";
            radioSideBtn->setEnabled(radioSideAvailable);
            radioSideBtn->setChecked(!clientSide);
            clientSideBtn->setChecked(clientSide);
            const QString why = radioSideAvailable
                ? QString()
                : tr("Unavailable: this radio can't record on its own side. "
                     "Recordings go to this computer.");
            radioSideBtn->setToolTip(why);
            radioSideBtn->setAccessibleDescription(why);
        };
        applyRecordingModeAvailability();
        connect(m_model, &RadioModel::connectionStateChanged, radioSideBtn,
                applyRecordingModeAvailability);

        connect(radioSideBtn, &QPushButton::clicked, this, [radioSideBtn, clientSideBtn]() {
            QSignalBlocker b(clientSideBtn);
            radioSideBtn->setChecked(true);
            clientSideBtn->setChecked(false);
            auto& s = AppSettings::instance();
            s.setValue("RecordingMode", "Radio");
            s.save();
        });
        connect(clientSideBtn, &QPushButton::clicked, this, [radioSideBtn, clientSideBtn]() {
            QSignalBlocker b(radioSideBtn);
            clientSideBtn->setChecked(true);
            radioSideBtn->setChecked(false);
            // Already in effect while Radio Side is dimmed; saving would erase
            // the Radio Side choice the next capable radio restores.
            if (!radioSideBtn->isEnabled())
                return;
            auto& s = AppSettings::instance();
            s.setValue("RecordingMode", "Client");
            s.save();
        });

        modeRow->addWidget(radioSideBtn);
        modeRow->addWidget(clientSideBtn);
        modeRow->addStretch();
        recLayout->addLayout(modeRow);

        // Recording directory (client-side only)
        auto* dirRow = new QHBoxLayout;
        auto* dirLabel = new QLabel("Save to:");
        applyLabelStyle(dirLabel);
        dirLabel->setFixedWidth(90);
        auto* dirEdit = new QLineEdit;
        dirEdit->setText(settings.value("QsoRecordingDir",
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            + "/AetherSDR/Recordings").toString());
        auto* browseBtn = new QPushButton("...");
        browseBtn->setFixedWidth(30);
        setButtonRole(browseBtn, ButtonRole::Select);
        connect(browseBtn, &QPushButton::clicked, this, [this, dirEdit]() {
            const QPointer<RadioSetupDialog> self(this);
            const QPointer<QLineEdit> dirEditGuard(dirEdit);
            QString dir = QFileDialog::getExistingDirectory(this, "Select Recording Directory",
                                                            dirEdit->text());
            if (self && dirEditGuard && !dir.isEmpty()) {
                dirEditGuard->setText(dir);
                auto& s = AppSettings::instance();
                s.setValue("QsoRecordingDir", dir);
                s.save();
            }
        });
        connect(dirEdit, &QLineEdit::editingFinished, this, [dirEdit]() {
            auto& s = AppSettings::instance();
            s.setValue("QsoRecordingDir", dirEdit->text());
            s.save();
        });
        dirRow->addWidget(dirLabel);
        dirRow->addWidget(dirEdit, 1);
        dirRow->addWidget(browseBtn);
        recLayout->addLayout(dirRow);

        // Auto-record on TX
        auto* autoRow = new QHBoxLayout;
        auto* autoCheck = new QCheckBox("Auto-record on TX");
        autoCheck->setChecked(settings.value("QsoRecordingAutoRecord", "False").toString() == "True");
        connect(autoCheck, &QCheckBox::toggled, this, [](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("QsoRecordingAutoRecord", on ? "True" : "False");
            s.save();
        });
        autoRow->addWidget(autoCheck);

        // Idle timeout
        auto* timeoutLabel = new QLabel("Idle timeout:");
        setTone(timeoutLabel, Tone::Hint);
        auto* timeoutSpin = new QSpinBox;
        timeoutSpin->setRange(10, 3600);
        timeoutSpin->setSuffix(" sec");
        timeoutSpin->setValue(settings.value("QsoRecordingIdleTimeout", "120").toInt());
        connect(timeoutSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int v) {
            auto& s = AppSettings::instance();
            s.setValue("QsoRecordingIdleTimeout", QString::number(v));
            s.save();
        });
        autoRow->addStretch();
        autoRow->addWidget(timeoutLabel);
        autoRow->addWidget(timeoutSpin);
        recLayout->addLayout(autoRow);

        vbox->addWidget(recGroup);
    }

    vbox->addStretch(1);
    return page;
}

// ── Filters tab ─────────────────────────────────────────────────────────────

QWidget* RadioSetupDialog::buildFiltersTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Model header
    {
        auto* hdr = new QHBoxLayout;
        hdr->addStretch(1);
        auto* modelLbl = new QLabel(m_model->model());
        setTone(modelLbl, Tone::Title);
        hdr->addWidget(modelLbl);
        vbox->addLayout(hdr);
    }


    // Filter sharpness sliders — taller groove + wider handle than the
    // canonical slider for the radio-setup dialog's heavier visual style.
    // Sizes kept site-local (deliberate emphasis); colours routed through
    // color.slider.* so live theme switching works and per-applet
    // overrides could retint these in future without per-call-site work.

    // Filter Options group
    {
        auto* group = new QGroupBox("Filter Options");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(8);
        // The sliders take the width; the two headings sit at their ends.
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(2, 1);

        // Column headers
        auto* lowLbl = new QLabel("Low Latency");
        applyLabelStyle(lowLbl);
        // Over the slider's two ends, which they name.
        lowLbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget(lowLbl, 0, 1);
        auto* sharpLbl = new QLabel("Sharp Filters");
        applyLabelStyle(sharpLbl);
        sharpLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(sharpLbl, 0, 2);

        struct FilterRow {
            const char* label;
            const char* modeCmd;   // voice, cw, digital
            int level;
            bool autoOn;
        };
        FilterRow rows[] = {
            {"Voice:",   "voice",   m_model->filterSharpnessVoice(),   m_model->filterSharpnessVoiceAuto()},
            {"CW:",      "cw",      m_model->filterSharpnessCw(),      m_model->filterSharpnessCwAuto()},
            {"Digital:", "digital", m_model->filterSharpnessDigital(), m_model->filterSharpnessDigitalAuto()},
        };

        for (int i = 0; i < 3; ++i) {
            auto& r = rows[i];
            int row = i + 1;

            auto* lbl = new QLabel(r.label);
            applyLabelStyle(lbl);
            grid->addWidget(lbl, row, 0);

            auto* slider = new GuardedSlider(Qt::Horizontal);
            slider->setRange(0, 3);
            slider->setValue(r.level);
            slider->setEnabled(!r.autoOn);
            grid->addWidget(slider, row, 1, 1, 2);

            auto* autoBtn = new QPushButton("Auto");
            autoBtn->setCheckable(true);
            autoBtn->setChecked(r.autoOn);
            setButtonRole(autoBtn, ButtonRole::Select);
            grid->addWidget(autoBtn, row, 3);

            QString mode = QString::fromLatin1(r.modeCmd);
            connect(slider, &QSlider::valueChanged, this, [this, mode](int v) {
                m_model->sendCommand(
                    QString("radio filter_sharpness %1 level=%2").arg(mode).arg(v));
            });
            connect(autoBtn, &QPushButton::toggled, this, [this, slider, mode](bool on) {
                slider->setEnabled(!on);
                m_model->sendCommand(
                    QString("radio filter_sharpness %1 auto_level=%2").arg(mode).arg(on ? 1 : 0));
            });
        }

        // Low Latency Digital checkbox: a filter option, in its group
        // rather than a box of its own with no title.
        auto* chk = new QCheckBox("Use Low Latency Filters for Digital Modes");
        chk->setChecked(m_model->lowLatencyDigital());
        connect(chk, &QCheckBox::toggled, this, [this](bool on) {
            m_model->sendCommand(
                QString("radio set low_latency_digital_modes=%1").arg(on ? 1 : 0));
        });
        grid->addWidget(chk, 4, 0, 1, 4);

        vbox->addWidget(group);
    }

    vbox->addStretch(1);
    return page;
}
QWidget* RadioSetupDialog::buildXvtrTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Model header
    {
        auto* hdr = new QHBoxLayout;
        hdr->addStretch(1);
        auto* modelLbl = new QLabel(m_model->model());
        setTone(modelLbl, Tone::Title);
        hdr->addWidget(modelLbl);
        vbox->addLayout(hdr);
    }

    // Sub-tabs: one per XVTR + a "+" tab to add new
    auto* xvtrTabs = new QTabWidget;

    auto buildXvtrPage = [this, xvtrTabs](int idx, const RadioModel::XvtrInfo& x) {
        auto* pg = new QWidget;
        auto* grid = new QGridLayout(pg);
        grid->setSpacing(6);

        auto addField = [&](int row, int col, const QString& label, const QString& value,
                             bool editable = true) -> QLineEdit* {
            auto* lbl = new QLabel(label);
            applyLabelStyle(lbl);
            grid->addWidget(lbl, row, col * 2);
            auto* edit = new QLineEdit(value);
            edit->setFixedWidth(100);
            edit->setReadOnly(!editable);
            grid->addWidget(edit, row, col * 2 + 1);
            return edit;
        };

        auto* nameEdit   = addField(0, 0, "Name:", x.name);
        auto* validLbl   = new QLabel(x.isValid ? "Valid" : "Invalid");
        setTone(validLbl, x.isValid ? Tone::Ok : Tone::Error);
        grid->addWidget(validLbl, 0, 3);

        auto* rfEdit     = addField(1, 0, "RF Freq (MHz):", QString::number(x.rfFreq, 'f', 3));
        auto* ifEdit     = addField(1, 1, "IF Freq (MHz):", QString::number(x.ifFreq, 'f', 3));
        auto* loEdit     = addField(2, 0, "LO Freq (MHz):", QString::number(x.rfFreq - x.ifFreq, 'f', 3), false);
        auto* errEdit    = addField(2, 1, "LO Error (MHz):", QString::number(x.loError, 'f', 6));
        auto* rxGainEdit = addField(3, 0, "RX Gain (dB):", QString::number(x.rxGain, 'f', 1));

        // RX Only toggle
        auto* rxOnlyLbl = new QLabel("RX Only:");
        applyLabelStyle(rxOnlyLbl);
        grid->addWidget(rxOnlyLbl, 3, 2);
        auto* rxOnlyBtn = new QPushButton(x.rxOnly ? "Enabled" : "Disabled");
        rxOnlyBtn->setCheckable(true);
        rxOnlyBtn->setChecked(x.rxOnly);
        connect(rxOnlyBtn, &QPushButton::toggled, this, [this, idx, rxOnlyBtn](bool on) {
            rxOnlyBtn->setText(on ? "Enabled" : "Disabled");
            m_model->sendCommand(
                QString("xvtr set %1 rx_only=%2").arg(idx).arg(on ? 1 : 0));
        });
        grid->addWidget(rxOnlyBtn, 3, 3);

        auto* maxPwrEdit = addField(4, 0, "Max Power (dBm):", QString::number(x.maxPower, 'f', 1));

        // Remove button
        auto* removeBtn = new QPushButton("Remove");
        setButtonRole(removeBtn, ButtonRole::Danger);
        connect(removeBtn, &QPushButton::clicked, pg, [this, idx, xvtrTabs, pg] {
            m_model->sendCommand(QString("xvtr remove %1").arg(idx));
            int tabIdx = xvtrTabs->indexOf(pg);
            if (tabIdx >= 0) xvtrTabs->removeTab(tabIdx);
        });
        grid->addWidget(removeBtn, 4, 3);

        auto maxPowerRange = [this, ifEdit] {
            return XvtrPolicy::maxPowerRangeFor(ifEdit->text().toDouble(), m_model->model());
        };
        auto* maxPwrValidator = new QDoubleValidator(maxPwrEdit);
        maxPwrValidator->setDecimals(2);
        maxPwrValidator->setNotation(QDoubleValidator::StandardNotation);
        auto updateMaxPowerValidator = [maxPwrValidator, maxPowerRange] {
            const XvtrPolicy::MaxPowerRange range = maxPowerRange();
            maxPwrValidator->setRange(range.minimumDbm, range.maximumDbm, 2);
        };
        updateMaxPowerValidator();
        maxPwrEdit->setValidator(maxPwrValidator);
        auto submitMaxPower = [this, maxPwrEdit, ifEdit, idx](bool sendWhenUnchanged) {
            bool ok = false;
            const double requested = maxPwrEdit->text().toDouble(&ok);
            if (!ok) {
                return;
            }

            const double clamped = XvtrPolicy::clampMaxPowerDbm(
                requested, ifEdit->text().toDouble(), m_model->model());
            maxPwrEdit->setText(QString::number(clamped, 'f', 2));
            if (!sendWhenUnchanged && qFuzzyCompare(requested + 1.0, clamped + 1.0)) {
                return;
            }

            m_model->sendCommand(
                QString("xvtr set %1 max_power=%2")
                    .arg(idx)
                    .arg(QString::number(clamped, 'f', 2)));
        };

        // Wire editable fields
        connect(nameEdit, &QLineEdit::editingFinished, this, [this, nameEdit, idx] {
            m_model->sendCommand(
                QString("xvtr set %1 name=%2").arg(idx).arg(nameEdit->text()));
        });
        auto updateLo = [rfEdit, ifEdit, loEdit] {
            double rf = rfEdit->text().toDouble();
            double ifF = ifEdit->text().toDouble();
            loEdit->setText(QString::number(rf - ifF, 'f', 3));
        };
        connect(rfEdit, &QLineEdit::editingFinished, this, [this, rfEdit, idx, updateLo] {
            m_model->sendCommand(
                QString("xvtr set %1 rf_freq=%2").arg(idx).arg(rfEdit->text()));
            updateLo();
        });
        connect(ifEdit, &QLineEdit::editingFinished, this,
                [this, ifEdit, idx, updateLo, updateMaxPowerValidator, submitMaxPower] {
            m_model->sendCommand(
                QString("xvtr set %1 if_freq=%2").arg(idx).arg(ifEdit->text()));
            updateLo();
            updateMaxPowerValidator();
            submitMaxPower(false);
        });
        connect(errEdit, &QLineEdit::editingFinished, this, [this, errEdit, idx] {
            m_model->sendCommand(
                QString("xvtr set %1 lo_error=%2").arg(idx).arg(errEdit->text()));
        });
        connect(rxGainEdit, &QLineEdit::editingFinished, this, [this, rxGainEdit, idx] {
            m_model->sendCommand(
                QString("xvtr set %1 rx_gain=%2").arg(idx).arg(rxGainEdit->text()));
        });
        connect(maxPwrEdit, &QLineEdit::editingFinished, this, [submitMaxPower] {
            submitMaxPower(true);
        });

        return pg;
    };

    // Add existing XVTR pages
    const auto& xvtrs = m_model->xvtrList();
    for (auto it = xvtrs.constBegin(); it != xvtrs.constEnd(); ++it) {
        xvtrTabs->addTab(buildXvtrPage(it.key(), it.value()),
                          it.value().name.isEmpty() ? QString::number(it.key()) : it.value().name);
    }

    // "+" tab to add new
    auto* addPage = new QWidget;
    auto* addVb = new QVBoxLayout(addPage);
    auto* addBtn = new QPushButton("Create New Transverter");
    connect(addBtn, &QPushButton::clicked, this, [this, xvtrTabs, buildXvtrPage] {
        m_model->sendCmdPublic("xvtr create",
            [this, xvtrTabs, buildXvtrPage](int code, const QString& /* body */) {
                if (code != 0) return;
                // Wait briefly for the radio's status update to arrive
                QTimer::singleShot(300, this, [this, xvtrTabs, buildXvtrPage] {
                    // Find the newest XVTR that doesn't have a tab yet
                    const auto& xvtrs = m_model->xvtrList();
                    for (auto it = xvtrs.constBegin(); it != xvtrs.constEnd(); ++it) {
                        // Check if we already have a tab for this index
                        bool found = false;
                        for (int t = 0; t < xvtrTabs->count() - 1; ++t) {
                            if (xvtrTabs->tabText(t) == it.value().name ||
                                xvtrTabs->tabText(t) == QString::number(it.key()))
                                found = true;
                        }
                        if (!found) {
                            QString tabName = it.value().name.isEmpty()
                                ? QString("New") : it.value().name;
                            int insertIdx = xvtrTabs->count() - 1; // before "+"
                            xvtrTabs->insertTab(insertIdx,
                                buildXvtrPage(it.key(), it.value()), tabName);
                            xvtrTabs->setCurrentIndex(insertIdx);
                        }
                    }
                });
            });
    });
    addVb->addWidget(addBtn, 0, Qt::AlignCenter);
    addVb->addStretch(1);
    xvtrTabs->addTab(addPage, "+");

    vbox->addWidget(xvtrTabs);
    vbox->addStretch(1);
    return page;
}

QWidget* RadioSetupDialog::buildAntennaNamesTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Model header
    {
        auto* hdr = new QHBoxLayout;
        hdr->addStretch(1);
        auto* modelLbl = new QLabel(m_model->model());
        setTone(modelLbl, Tone::Title);
        hdr->addWidget(modelLbl);
        vbox->addLayout(hdr);
    }

    auto* group = new QGroupBox("Antenna Names");
    auto* groupLayout = new QVBoxLayout(group);
    groupLayout->setSpacing(6);

    auto* rowsWidget = new QWidget;
    rowsWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto* grid = new QGridLayout(rowsWidget);
    grid->setContentsMargins(8, 2, 8, 2);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->setAlignment(Qt::AlignTop);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(rowsWidget);
    // As tall as its rows (a scroll area caps that), not the rest of the
    // page: one antenna is one row, not a box down to the window's foot.
    scroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    groupLayout->addWidget(scroll);
    vbox->addWidget(group, 0);

    QVBoxLayout* kiwiRowsLayout = nullptr;
    QVBoxLayout* kiwiLayout = nullptr;

#ifdef HAVE_KEYCHAIN
    const QString kiwiPasswordHelpText =
        QStringLiteral("Configure receive-only KiwiSDR servers. Passwords "
                       "are saved separately for each receiver in the "
                       "operating system credential store when available. "
                       "The status below each password confirms the result.");
    const QString kiwiPasswordDescription =
        QStringLiteral("Optional password for this KiwiSDR receiver. Type "
                       "over the current value to replace it; the storage "
                       "status below confirms whether the operating system "
                       "credential store accepted it.");
#else
    const QString kiwiPasswordHelpText =
        QStringLiteral("Configure receive-only KiwiSDR servers. This build "
                       "keeps passwords only for the current session.");
    const QString kiwiPasswordDescription =
        QStringLiteral("Optional password for this KiwiSDR receiver. This "
                       "build keeps it only for the current session.");
#endif

    if (m_kiwiSdrManager) {
        auto* kiwiGroup = new QGroupBox("KiwiSDR RX Antennas");
        kiwiLayout = new QVBoxLayout(kiwiGroup);
        kiwiLayout->setSpacing(6);

        auto* kiwiHelp = new QLabel(
            kiwiPasswordHelpText);
        kiwiHelp->setWordWrap(true);
        kiwiHelp->setAccessibleName("KiwiSDR receiver configuration help");
        setTone(kiwiHelp, Tone::Hint);
        kiwiLayout->addWidget(kiwiHelp);

        auto* kiwiCredentialNotice = new QLabel;
        kiwiCredentialNotice->setWordWrap(true);
        kiwiCredentialNotice->setAccessibleName(
            "KiwiSDR credential storage notice");
        kiwiCredentialNotice->setVisible(false);
        setTone(kiwiCredentialNotice, Tone::Error);
        kiwiLayout->addWidget(kiwiCredentialNotice);
        connect(
            m_kiwiSdrManager,
            &KiwiSdrManager::profilePasswordPersistenceChanged,
            kiwiCredentialNotice,
            [kiwiCredentialNotice](
                const QString& id, KiwiSdrPasswordPersistenceState state,
                const QString& detail) {
                if (state != KiwiSdrPasswordPersistenceState::Error) {
                    if (kiwiCredentialNotice->property("profileId").toString()
                        == id) {
                        kiwiCredentialNotice->clear();
                        kiwiCredentialNotice->setVisible(false);
                    }
                    return;
                }
                kiwiCredentialNotice->setProperty("profileId", id);
                kiwiCredentialNotice->setText(detail);
                kiwiCredentialNotice->setAccessibleDescription(detail);
                kiwiCredentialNotice->setVisible(true);
            });

        auto* kiwiScroll = new QScrollArea;
        kiwiScroll->setWidgetResizable(true);
        kiwiScroll->setFrameShape(QFrame::NoFrame);
        kiwiScroll->setMinimumHeight(190);
        kiwiScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        kiwiScroll->setAccessibleName("KiwiSDR RX antennas");
        kiwiScroll->setAccessibleDescription(
            "Configured KiwiSDR receive-only antenna endpoints.");

        auto* kiwiRowsWidget = new QWidget;
        kiwiRowsWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        kiwiRowsLayout = new QVBoxLayout(kiwiRowsWidget);
        kiwiRowsLayout->setContentsMargins(8, 4, 8, 4);
        kiwiRowsLayout->setSpacing(6);
        kiwiRowsLayout->setAlignment(Qt::AlignTop);
        kiwiScroll->setWidget(kiwiRowsWidget);
        kiwiLayout->addWidget(kiwiScroll);

        vbox->addWidget(kiwiGroup, 0);
    }

    // Antenna display names are intentionally local to AetherSDR. FlexLib exposes
    // canonical RX/TX antenna lists and rxant/txant setters, but no verified
    // writable display-name API; radio commands must keep using ANT1/XVTR/etc.
    auto refresh = std::make_shared<std::function<void()>>();
    auto scheduleRefresh = [this, refresh] {
        QTimer::singleShot(0, this, [refresh] {
            if (*refresh)
                (*refresh)();
        });
    };

    auto wireSlice = [this, scheduleRefresh](SliceModel* slice) {
        if (!slice)
            return;
        connect(slice, &SliceModel::rxAntennaChanged, this,
                [scheduleRefresh](const QString&) { scheduleRefresh(); });
        connect(slice, &SliceModel::txAntennaChanged, this,
                [scheduleRefresh](const QString&) { scheduleRefresh(); });
        connect(slice, &SliceModel::rxAntennaListChanged, this,
                [scheduleRefresh](const QStringList&) { scheduleRefresh(); });
        connect(slice, &SliceModel::txAntennaListChanged, this,
                [scheduleRefresh](const QStringList&) { scheduleRefresh(); });
    };

    *refresh = [this, grid] {
        while (QLayoutItem* item = grid->takeAt(0)) {
            if (QWidget* w = item->widget())
                w->deleteLater();
            delete item;
        }

        auto addHeader = [grid](const QString& text, int col) {
            auto* lbl = new QLabel(text);
            setTone(lbl, Tone::Heading);
            grid->addWidget(lbl, 0, col);
        };
        addHeader("Port", 0);
        addHeader("Custom Name", 1);
        addHeader("Preview", 2);

        const QStringList tokens = m_model->knownAntennaTokens();
        if (tokens.isEmpty()) {
            auto* empty = new QLabel("Waiting for antenna ports from the radio.");
            setTone(empty, Tone::Hint);
            grid->addWidget(empty, 1, 0, 1, 4);
            return;
        }

        int row = 1;
        for (const QString& token : tokens) {
            auto* port = makeValueLabel(token);
            grid->addWidget(port, row, 0);

            auto* edit = new QLineEdit(m_model->antennaAlias(token));
            edit->setMaxLength(16);
            edit->setPlaceholderText(token);
            grid->addWidget(edit, row, 1);

            const bool disambiguate =
                m_model->antennaAliasNeedsDisambiguation(token, tokens);
            auto* preview = new QLabel(m_model->antennaDisplayName(token, disambiguate));
            applyLabelStyle(preview);
            grid->addWidget(preview, row, 2);

            auto* clearBtn = new QPushButton("Clear");
            grid->addWidget(clearBtn, row, 3);

            connect(edit, &QLineEdit::textChanged, this,
                    [preview, token](const QString& text) {
                const QString alias = text.trimmed();
                preview->setText(alias.isEmpty() ? token : alias);
            });
            connect(edit, &QLineEdit::editingFinished, this, [this, edit, token] {
                m_model->setAntennaAlias(token, edit->text());
            });
            connect(clearBtn, &QPushButton::clicked, this, [this, edit, token] {
                edit->clear();
                m_model->clearAntennaAlias(token);
            });
            ++row;
        }
    };

    for (SliceModel* slice : m_model->slices())
        wireSlice(slice);
    connect(m_model, &RadioModel::sliceAdded, this,
            [wireSlice, scheduleRefresh](SliceModel* slice) {
        wireSlice(slice);
        scheduleRefresh();
    });
    connect(m_model, &RadioModel::antListChanged, this,
            [scheduleRefresh](const QStringList&) { scheduleRefresh(); });
    connect(m_model, &RadioModel::antennaAliasesChanged, this, scheduleRefresh);

    auto refreshKiwi = std::make_shared<std::function<void()>>();
    if (m_kiwiSdrManager && kiwiRowsLayout) {
        auto* kiwiTelemetryRefreshTimer = new QTimer(this);
        kiwiTelemetryRefreshTimer->setSingleShot(true);
        kiwiTelemetryRefreshTimer->setInterval(150);
        connect(kiwiTelemetryRefreshTimer, &QTimer::timeout,
                this, [refreshKiwi] {
            if (*refreshKiwi) {
                (*refreshKiwi)();
            }
        });

        auto refreshKiwiNow = [refreshKiwi, kiwiTelemetryRefreshTimer] {
            kiwiTelemetryRefreshTimer->stop();
            if (*refreshKiwi) {
                (*refreshKiwi)();
            }
        };

        auto stateText = [this](const QString& id) {
            const KiwiSdrClient::State state = m_kiwiSdrManager->state(id);
            QString base;
            switch (state) {
            case KiwiSdrClient::State::Disconnected:
                base = QStringLiteral("Disconnected");
                break;
            case KiwiSdrClient::State::Connecting:
                base = QStringLiteral("Connecting");
                break;
            case KiwiSdrClient::State::Connected:
                base = QStringLiteral("Connected");
                break;
            case KiwiSdrClient::State::Busy:
                base = QStringLiteral("Busy");
                break;
            case KiwiSdrClient::State::Waiting:
                base = QStringLiteral("Waiting");
                break;
            case KiwiSdrClient::State::Camping:
                base = QStringLiteral("Monitoring");
                break;
            case KiwiSdrClient::State::CampDisconnected:
                base = QStringLiteral("Camp ended");
                break;
            case KiwiSdrClient::State::Error:
                base = QStringLiteral("Error");
                break;
            }
            const QString detail = m_kiwiSdrManager->stateDetail(id).trimmed();
            if (!detail.isEmpty()
                && state != KiwiSdrClient::State::Connected
                && state != KiwiSdrClient::State::Connecting) {
                base = QStringLiteral("%1\n%2").arg(base, detail);
            }
            const QString metadata = kiwiSetupMetadataSummary(
                m_kiwiSdrManager, id);
            if (!metadata.isEmpty()) {
                base = QStringLiteral("%1\n%2")
                           .arg(base.isEmpty()
                                    ? QStringLiteral("Disconnected")
                                    : base,
                                metadata);
            }
            return base.isEmpty() ? QStringLiteral("Disconnected") : base;
        };

        auto styleKiwiEdit = [](QLineEdit* edit) {
            edit->setMinimumHeight(24);
        };

        auto styleKiwiCombo = [](QComboBox* combo) {
            applyComboStyle(combo);
            combo->setMinimumHeight(24);
        };

        // Receiver-family selector shared by the configured and new rows:
        // index 0 = KiwiSDR, index 1 = Web-888 (docs/web888-cleanroom-design.md).
        auto fillReceiverTypeCombo = [](QComboBox* combo) {
            combo->addItem(QStringLiteral("KiwiSDR"));
            combo->addItem(QStringLiteral("Web-888"));
        };
        auto receiverTypeComboIndex =
            [](KiwiSdrProtocol::KiwiSdrReceiverFamily family) {
                return family == KiwiSdrProtocol::KiwiSdrReceiverFamily::Web888
                    ? 1
                    : 0;
            };
        auto receiverTypeComboFamily =
            [](int index) {
                return index == 1
                    ? KiwiSdrProtocol::KiwiSdrReceiverFamily::Web888
                    : KiwiSdrProtocol::KiwiSdrReceiverFamily::Kiwi;
            };

        auto styleKiwiButton = [](QPushButton* button) {
            button->setMinimumWidth(96);
            button->setAutoDefault(false);
        };

        auto styleKiwiIconButton = [](QPushButton* button) {
            button->setFixedSize(30, 26);
            button->setAutoDefault(false);
        };

        *refreshKiwi = [this, kiwiRowsLayout, stateText, styleKiwiEdit,
                        styleKiwiButton, styleKiwiIconButton, styleKiwiCombo,
                        fillReceiverTypeCombo, receiverTypeComboIndex,
                        receiverTypeComboFamily,
                        kiwiPasswordDescription] {
            while (QLayoutItem* item = kiwiRowsLayout->takeAt(0)) {
                if (QWidget* widget = item->widget()) {
                    widget->deleteLater();
                }
                delete item;
            }

            const QVector<KiwiSdrAntennaProfile> profiles =
                m_kiwiSdrManager->profiles();
            auto addSectionHeader = [kiwiRowsLayout](const QString& text) {
                auto* header = new QLabel(text);
                setTone(header, Tone::Heading);
                kiwiRowsLayout->addWidget(header);
            };
            auto addFieldLabel = [](QGridLayout* layout, const QString& text,
                                    int column) {
                auto* label = new QLabel(text);
                setTone(label, Tone::Heading);
                layout->addWidget(label, 1, column);
            };
            auto passwordPersistenceText = [](KiwiSdrPasswordPersistenceState state,
                                              const QString& detail) {
                switch (state) {
                case KiwiSdrPasswordPersistenceState::Loading:
                    return QStringLiteral("Loading secure password…");
                case KiwiSdrPasswordPersistenceState::NoPassword:
                    return QStringLiteral("No password stored");
                case KiwiSdrPasswordPersistenceState::Saving:
                    return QStringLiteral("Saving securely…");
                case KiwiSdrPasswordPersistenceState::Stored:
                    return QStringLiteral("Stored securely");
                case KiwiSdrPasswordPersistenceState::SessionOnly:
                    return QStringLiteral("Current session only");
                case KiwiSdrPasswordPersistenceState::Error:
                    return detail.isEmpty()
                        ? QStringLiteral("Password was not stored")
                        : detail;
                }
                return QString();
            };

            addSectionHeader("Configured Receivers");
            if (profiles.isEmpty()) {
                auto* empty = new QLabel("No KiwiSDR receivers configured.");
                empty->setAccessibleName("No configured KiwiSDR receivers");
                setTone(empty, Tone::Hint);
                kiwiRowsLayout->addWidget(empty);
            }

            for (const KiwiSdrAntennaProfile& profile : profiles) {

                auto* rowFrame = new QFrame;
                rowFrame->setObjectName("kiwiAntennaRow");
                rowFrame->setAccessibleName(
                    QStringLiteral("KiwiSDR receiver %1").arg(profile.name));
                auto* rowLayout = new QGridLayout(rowFrame);
                rowLayout->setContentsMargins(10, 8, 10, 8);
                rowLayout->setHorizontalSpacing(8);
                rowLayout->setVerticalSpacing(4);
                rowLayout->setColumnStretch(0, 1);
                rowLayout->setColumnStretch(1, 1);
                rowLayout->setColumnStretch(2, 1);

                auto* title = new QLabel(profile.name);
                title->setAccessibleName("KiwiSDR receiver name");
                setTone(title, Tone::Title);
                rowLayout->addWidget(title, 0, 0, 1, 2);

                const KiwiSdrClient::State kiwiState =
                    m_kiwiSdrManager->state(profile.id);
                auto* status = new QLabel(stateText(profile.id));
                status->setAccessibleName("KiwiSDR antenna status");
                status->setAccessibleDescription(status->text());
                Tone statusTone = Tone::Hint;
                if (kiwiState == KiwiSdrClient::State::Connected
                    || kiwiState == KiwiSdrClient::State::Camping) {
                    statusTone = Tone::Ok;
                } else if (kiwiState == KiwiSdrClient::State::Error) {
                    statusTone = Tone::Error;
                } else if (kiwiState == KiwiSdrClient::State::Connecting
                           || kiwiState == KiwiSdrClient::State::Waiting) {
                    statusTone = Tone::Accent;
                }
                setTone(status, statusTone);
                status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                status->setWordWrap(true);
                status->setToolTip(status->text());
                rowLayout->addWidget(status, 0, 2, 1, 2);

                addFieldLabel(rowLayout, "Name", 0);
                addFieldLabel(rowLayout, "Server", 1);
                addFieldLabel(rowLayout, "Password", 2);
                addFieldLabel(rowLayout, "Type", 3);

                auto* nameEdit = new QLineEdit(profile.name);
                nameEdit->setMaxLength(16);
                nameEdit->setAccessibleName("KiwiSDR antenna name");
                nameEdit->setAccessibleDescription(
                    "Required display name for this KiwiSDR receive antenna.");
                styleKiwiEdit(nameEdit);
                rowLayout->addWidget(nameEdit, 2, 0);

                auto* endpointEdit = new QLineEdit(profile.endpoint);
                endpointEdit->setAccessibleName("KiwiSDR server");
                endpointEdit->setAccessibleDescription(
                    "Hostname or hostname:port for this KiwiSDR endpoint.");
                styleKiwiEdit(endpointEdit);
                rowLayout->addWidget(endpointEdit, 2, 1);

                auto* passwordEdit = new QLineEdit(
                    m_kiwiSdrManager->profilePassword(profile.id));
                passwordEdit->setEchoMode(QLineEdit::Password);
                passwordEdit->setMaxLength(256);
                passwordEdit->setObjectName(
                    QStringLiteral("kiwiPassword_%1").arg(profile.id));
                passwordEdit->setAccessibleName("KiwiSDR password");
                passwordEdit->setAccessibleDescription(
                    kiwiPasswordDescription);
                if (!m_kiwiSdrManager->isProfilePasswordLoaded(profile.id)) {
                    passwordEdit->setPlaceholderText("Loading secure password…");
                    passwordEdit->setEnabled(false);
                }
                styleKiwiEdit(passwordEdit);
                rowLayout->addWidget(passwordEdit, 2, 2);

                auto* typeCombo = new GuardedComboBox;
                fillReceiverTypeCombo(typeCombo);
                typeCombo->setCurrentIndex(receiverTypeComboIndex(profile.family));
                typeCombo->setAccessibleName("KiwiSDR receiver type");
                typeCombo->setAccessibleDescription(
                    "Receiver family for this endpoint: a KiwiSDR or a "
                    "Web-888, which speaks the same protocol with small "
                    "differences.");
                styleKiwiCombo(typeCombo);
                rowLayout->addWidget(typeCombo, 2, 3);

                const KiwiSdrPasswordPersistenceState persistenceState =
                    m_kiwiSdrManager->profilePasswordPersistenceState(
                        profile.id);
                auto* passwordStatus = new QLabel(passwordPersistenceText(
                    persistenceState,
                    m_kiwiSdrManager->profilePasswordPersistenceDetail(
                        profile.id)));
                passwordStatus->setObjectName(
                    QStringLiteral("kiwiPasswordStatus_%1").arg(profile.id));
                passwordStatus->setAccessibleName(
                    "KiwiSDR password storage status");
                passwordStatus->setAccessibleDescription(
                    passwordStatus->text());
                passwordStatus->setWordWrap(true);
                setTone(passwordStatus, persistenceState == KiwiSdrPasswordPersistenceState::Error ? Tone::Error : Tone::Hint);
                rowLayout->addWidget(passwordStatus, 3, 2, 1, 2);
                connect(
                    m_kiwiSdrManager,
                    &KiwiSdrManager::profilePasswordChanged,
                    passwordEdit,
                    [manager = m_kiwiSdrManager, profileId = profile.id,
                     passwordEdit](const QString& changedId) {
                        if (changedId != profileId) {
                            return;
                        }
                        const QSignalBlocker blocker(passwordEdit);
                        passwordEdit->setText(
                            manager->profilePassword(profileId));
                        passwordEdit->setPlaceholderText(QString());
                        passwordEdit->setEnabled(true);
                    });
                connect(
                    m_kiwiSdrManager,
                    &KiwiSdrManager::profilePasswordPersistenceChanged,
                    passwordStatus,
                    [profileId = profile.id, passwordStatus,
                     passwordPersistenceText](
                        const QString& changedId,
                        KiwiSdrPasswordPersistenceState state,
                        const QString& detail) {
                        if (changedId != profileId) {
                            return;
                        }
                        const QString text =
                            passwordPersistenceText(state, detail);
                        passwordStatus->setText(text);
                        passwordStatus->setAccessibleDescription(text);
                        setTone(passwordStatus, state == KiwiSdrPasswordPersistenceState::Error ? Tone::Error : Tone::Hint);
                    });

                auto* autoCheck = new QCheckBox;
                autoCheck->setText("Auto-connect");
                autoCheck->setChecked(profile.autoConnect);
                autoCheck->setAccessibleName("Auto connect KiwiSDR antenna");
                rowLayout->addWidget(autoCheck, 4, 0, 1, 2, Qt::AlignLeft);

                auto* keepTxAudioCheck = new QCheckBox;
                keepTxAudioCheck->setText("Keep audio during TX");
                keepTxAudioCheck->setChecked(profile.keepAudioDuringTx);
                keepTxAudioCheck->setAccessibleName(
                    "Keep KiwiSDR audio during transmit");
                keepTxAudioCheck->setToolTip(
                    "Keep playing this receiver's audio while transmitting.\n"
                    "Off: its audio is silenced during TX and resumes "
                    "immediately at unkey.");
                rowLayout->addWidget(keepTxAudioCheck, 5, 0, 1, 2,
                                     Qt::AlignLeft);

                auto* resumeDelayCheck = new QCheckBox;
                resumeDelayCheck->setText("Resume audio after TX delay");
                resumeDelayCheck->setChecked(profile.resumeAudioAfterTxDelay);
                resumeDelayCheck->setEnabled(!profile.keepAudioDuringTx);
                resumeDelayCheck->setAccessibleName(
                    "Resume KiwiSDR audio after transmit delay");
                resumeDelayCheck->setToolTip(
                    "After unkeying, wait out this receiver's stream delay "
                    "before unmuting, so you rejoin on audio received after "
                    "your transmission ended instead of hearing your own "
                    "delayed TX tail.\nNo effect while \"Keep audio during "
                    "TX\" is on.");
                rowLayout->addWidget(resumeDelayCheck, 6, 0, 1, 2,
                                     Qt::AlignLeft);

                const bool activeSession =
                    kiwiState == KiwiSdrClient::State::Connecting
                    || kiwiState == KiwiSdrClient::State::Waiting
                    || KiwiSdrClient::stateHasReceiveAudio(kiwiState);
                auto* connectButton =
                    new QPushButton(activeSession ? "Disconnect" : "Connect");
                connectButton->setAccessibleName(
                    activeSession ? "Disconnect KiwiSDR antenna"
                                  : "Connect KiwiSDR antenna");
                styleKiwiButton(connectButton);
                rowLayout->addWidget(connectButton, 4, 2);

                auto* removeButton = new QPushButton;
                removeButton->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
                removeButton->setToolTip("Remove");
                removeButton->setAccessibleName("Remove KiwiSDR antenna");
                styleKiwiIconButton(removeButton);
                rowLayout->addWidget(removeButton, 4, 3);
                kiwiRowsLayout->addWidget(rowFrame);

                auto updateProfile = [this, profile, nameEdit, endpointEdit,
                                      typeCombo, receiverTypeComboFamily,
                                      autoCheck, keepTxAudioCheck,
                                      resumeDelayCheck] {
                    const QString name = nameEdit->text().trimmed();
                    const QString endpoint =
                        KiwiSdrClient::normalizeEndpoint(endpointEdit->text());
                    if (name.isEmpty()) {
                        QSignalBlocker blocker(nameEdit);
                        nameEdit->setText(profile.name);
                        return;
                    }
                    if (endpoint.isEmpty()) {
                        QSignalBlocker blocker(endpointEdit);
                        endpointEdit->setText(profile.endpoint);
                        return;
                    }
                    if (endpointEdit->text() != endpoint) {
                        QSignalBlocker blocker(endpointEdit);
                        endpointEdit->setText(endpoint);
                    }
                    KiwiSdrAntennaProfile updated = profile;
                    updated.name = name;
                    updated.endpoint = endpoint;
                    updated.family = receiverTypeComboFamily(
                        typeCombo->currentIndex());
                    updated.autoConnect = autoCheck->isChecked();
                    updated.keepAudioDuringTx = keepTxAudioCheck->isChecked();
                    updated.resumeAudioAfterTxDelay =
                        resumeDelayCheck->isChecked();
                    m_kiwiSdrManager->updateProfile(updated);
                };
                connect(nameEdit, &QLineEdit::editingFinished,
                        this, updateProfile);
                connect(nameEdit, &QLineEdit::returnPressed,
                        this, updateProfile);
                connect(endpointEdit, &QLineEdit::editingFinished,
                        this, updateProfile);
                connect(endpointEdit, &QLineEdit::returnPressed,
                        this, updateProfile);
                connect(passwordEdit, &QLineEdit::editingFinished,
                        this, [this, profile, passwordEdit] {
                    m_kiwiSdrManager->setProfilePassword(
                        profile.id, passwordEdit->text());
                });
                connect(autoCheck, &QCheckBox::toggled,
                        this, [updateProfile](bool) { updateProfile(); });
                connect(keepTxAudioCheck, &QCheckBox::toggled,
                        this, [updateProfile, resumeDelayCheck](bool on) {
                    resumeDelayCheck->setEnabled(!on);
                    updateProfile();
                });
                connect(resumeDelayCheck, &QCheckBox::toggled,
                        this, [updateProfile](bool) { updateProfile(); });
                connect(typeCombo, &QComboBox::currentIndexChanged,
                        this, [updateProfile](int) { updateProfile(); });
                connect(connectButton, &QPushButton::clicked,
                        this, [this, profile, activeSession] {
                    if (activeSession) {
                        m_kiwiSdrManager->disconnectProfile(profile.id);
                    } else {
                        m_kiwiSdrManager->connectProfile(profile.id);
                    }
                });
                connect(removeButton, &QPushButton::clicked,
                        this, [this, profile] {
                    m_kiwiSdrManager->removeProfile(profile.id);
                });
            }

            addSectionHeader("Add Receiver");
            auto* rowFrame = new QFrame;
            rowFrame->setObjectName("kiwiAntennaRow");
            auto* rowLayout = new QGridLayout(rowFrame);
            rowLayout->setContentsMargins(10, 8, 10, 8);
            rowLayout->setHorizontalSpacing(8);
            rowLayout->setVerticalSpacing(4);
            rowLayout->setColumnStretch(0, 1);
            rowLayout->setColumnStretch(1, 1);
            rowLayout->setColumnStretch(2, 1);

            addFieldLabel(rowLayout, "Name", 0);
            addFieldLabel(rowLayout, "Server", 1);
            addFieldLabel(rowLayout, "Password", 2);
            addFieldLabel(rowLayout, "Type", 3);

            auto* nameEdit = new QLineEdit;
            nameEdit->setMaxLength(16);
            nameEdit->setPlaceholderText("Custom Name");
            nameEdit->setAccessibleName("New KiwiSDR antenna name");
            nameEdit->setAccessibleDescription(
                "Required display name for the new KiwiSDR receive antenna.");
            styleKiwiEdit(nameEdit);
            rowLayout->addWidget(nameEdit, 2, 0);

            auto* endpointEdit = new QLineEdit;
            endpointEdit->setPlaceholderText("host:8073");
            endpointEdit->setAccessibleName("New KiwiSDR server");
            endpointEdit->setAccessibleDescription(
                "Hostname or hostname:port for the new KiwiSDR receive antenna.");
            styleKiwiEdit(endpointEdit);
            rowLayout->addWidget(endpointEdit, 2, 1);

            auto* passwordEdit = new QLineEdit;
            passwordEdit->setEchoMode(QLineEdit::Password);
            passwordEdit->setMaxLength(256);
            passwordEdit->setObjectName("newKiwiPassword");
            passwordEdit->setAccessibleName("New KiwiSDR password");
            passwordEdit->setAccessibleDescription(
                kiwiPasswordDescription);
            styleKiwiEdit(passwordEdit);
            rowLayout->addWidget(passwordEdit, 2, 2);

            auto* typeCombo = new GuardedComboBox;
            fillReceiverTypeCombo(typeCombo);
            typeCombo->setAccessibleName("New KiwiSDR receiver type");
            typeCombo->setAccessibleDescription(
                "Receiver family for the new endpoint: a KiwiSDR or a "
                "Web-888, which speaks the same protocol with small "
                "differences.");
            styleKiwiCombo(typeCombo);
            rowLayout->addWidget(typeCombo, 2, 3);

            auto* autoCheck = new QCheckBox;
            autoCheck->setText("Auto-connect");
            autoCheck->setAccessibleName("Auto connect new KiwiSDR antenna");
            rowLayout->addWidget(autoCheck, 3, 0, Qt::AlignLeft);

            auto committed = std::make_shared<bool>(false);
            auto commitNewRow = [this, nameEdit, endpointEdit, passwordEdit,
                                 typeCombo, receiverTypeComboFamily,
                                 autoCheck, committed] {
                if (*committed) {
                    return;
                }
                const QString name = nameEdit->text().trimmed();
                const QString endpoint =
                    KiwiSdrClient::normalizeEndpoint(endpointEdit->text());
                if (name.isEmpty() || endpoint.isEmpty()) {
                    return;
                }
                *committed = true;
                const QString id = m_kiwiSdrManager->addProfile(
                    name, endpoint, receiverTypeComboFamily(
                                        typeCombo->currentIndex()));
                if (id.isEmpty()) {
                    *committed = false;
                    return;
                }
                m_kiwiSdrManager->setProfilePassword(id, passwordEdit->text());
                KiwiSdrAntennaProfile profile = m_kiwiSdrManager->profile(id);
                profile.autoConnect = autoCheck->isChecked();
                m_kiwiSdrManager->updateProfile(profile);
            };

            // Browse the public KiwiSDR directory to fill in a receiver. Only
            // API-permitting receivers are listed (web-only operators honored).
            // Picking one fills the fields; Add receiver commits the profile.
            auto* browseButton = new QPushButton("Browse Public…");
            browseButton->setAccessibleName("Browse public KiwiSDR receivers");
            browseButton->setAccessibleDescription(
                "Choose from the public KiwiSDR directory; receivers whose "
                "operator disabled the external API are not shown.");
            styleKiwiButton(browseButton);
            rowLayout->addWidget(browseButton, 3, 1);
            connect(browseButton, &QPushButton::clicked, this,
                    [this, nameEdit, endpointEdit] {
                const QPointer<RadioSetupDialog> self(this);
                const QPointer<QLineEdit> nameEditGuard(nameEdit);
                const QPointer<QLineEdit> endpointEditGuard(endpointEdit);
                ScopedChildWidget<KiwiPublicReceiverPicker> pickerOwner(this);
                KiwiPublicReceiverPicker& picker = *pickerOwner.get();
                const int result = picker.exec();
                if (!self || !nameEditGuard || !endpointEditGuard || !pickerOwner
                    || result != QDialog::Accepted) {
                    return;
                }
                const QString endpoint = picker.selectedEndpoint();
                const QString name = picker.selectedName();
                if (!endpoint.isEmpty()) {
                    endpointEditGuard->setText(endpoint);
                    if (!self || !nameEditGuard) {
                        return;
                    }
                    if (nameEditGuard->text().trimmed().isEmpty()) {
                        nameEditGuard->setText(name);
                    }
                }
            });

            auto* addButton = new QPushButton("Add Receiver");
            addButton->setAccessibleName("Add KiwiSDR receiver");
            styleKiwiButton(addButton);
            rowLayout->addWidget(addButton, 3, 2);
            connect(addButton, &QPushButton::clicked, this, commitNewRow);
            kiwiRowsLayout->addWidget(rowFrame);

            connect(nameEdit, &QLineEdit::returnPressed,
                    this, commitNewRow);
            connect(endpointEdit, &QLineEdit::returnPressed,
                    this, commitNewRow);
            connect(passwordEdit, &QLineEdit::returnPressed,
                    this, commitNewRow);

            kiwiRowsLayout->addStretch(1);
        };

        connect(m_kiwiSdrManager, &KiwiSdrManager::profilesChanged,
                this, refreshKiwiNow);
        connect(m_kiwiSdrManager, &KiwiSdrManager::profileStateChanged,
                this, [refreshKiwiNow](const QString&,
                                        KiwiSdrClient::State,
                                        const QString&) {
            refreshKiwiNow();
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::profileTelemetryChanged,
                this, [kiwiTelemetryRefreshTimer](const QString&,
                                                  const KiwiSdrReceiverTelemetry&) {
            if (!kiwiTelemetryRefreshTimer->isActive()) {
                kiwiTelemetryRefreshTimer->start();
            }
        });

        // Import/export the receiver list as a CSV (#4586) — passwords are
        // never included (they live in the OS credential store), so a
        // password-protected receiver needs its password re-entered after
        // import. Remembers its own last-used folder, independent of any
        // other CSV import/export elsewhere in the app (mirrors
        // ShortcutDialog's pattern). Reuses styleKiwiButton (declared above
        // for the per-row Connect/Add-receiver buttons) instead of adding a
        // fresh setStyleSheet() call site the colour-audit ratchet counts.
        auto kiwiTransferDirectory = [] {
            const QString saved = AppSettings::instance()
                .value(QStringLiteral("KiwiSdrImportExportPath"), QString())
                .toString();
            if (!saved.isEmpty() && QDir(saved).exists()) {
                return saved;
            }
            const QString docs = QStandardPaths::writableLocation(
                QStandardPaths::DocumentsLocation);
            return docs.isEmpty() ? QDir::homePath() : docs;
        };
        auto rememberKiwiTransferDirectory = [](const QString& path) {
            const QFileInfo info(path);
            if (!info.absolutePath().isEmpty()) {
                AppSettings::instance().setValue(
                    QStringLiteral("KiwiSdrImportExportPath"), info.absolutePath());
            }
        };

        auto* kiwiTransferRow = new QHBoxLayout;
        kiwiTransferRow->addStretch(1);

        auto* kiwiImportBtn = new QPushButton("Import...");
        kiwiImportBtn->setObjectName(QStringLiteral("kiwiImportButton"));
        kiwiImportBtn->setAccessibleName("Import KiwiSDR receivers");
        kiwiImportBtn->setAccessibleDescription(
            "Import KiwiSDR receivers from a CSV file. A receiver whose "
            "endpoint matches one already saved is updated in place.");
        styleKiwiButton(kiwiImportBtn);
        connect(kiwiImportBtn, &QPushButton::clicked, this,
                [this, kiwiTransferDirectory, rememberKiwiTransferDirectory] {
            const QPointer<RadioSetupDialog> self(this);
            const QPointer<KiwiSdrManager> manager(m_kiwiSdrManager);
            ScopedChildWidget<QFileDialog> dialogOwner(
                this, QStringLiteral("Import KiwiSDR Receivers"),
                kiwiTransferDirectory(), QStringLiteral("CSV Files (*.csv)"));
            QFileDialog& dialog = *dialogOwner.get();
            dialog.setAcceptMode(QFileDialog::AcceptOpen);
            dialog.setFileMode(QFileDialog::ExistingFile);
            dialog.setDefaultSuffix(QStringLiteral("csv"));
            const int dialogResult = dialog.exec();
            if (!self || !manager || !dialogOwner || dialogResult != QDialog::Accepted
                || dialog.selectedFiles().isEmpty()) {
                return;
            }
            const QString path = dialog.selectedFiles().first();
            rememberKiwiTransferDirectory(path);

            const KiwiSdrCsvImportResult result =
                manager->importFromFile(path);
            if (!self || !manager) {
                return;
            }
            if (!result.ok() && result.addedCount == 0 && result.mergedCount == 0) {
                ScopedChildWidget<QMessageBox> boxOwner(
                    QMessageBox::Warning, QStringLiteral("Import KiwiSDR Receivers"),
                    QStringLiteral("No receivers were imported from %1.")
                        .arg(QFileInfo(path).fileName()),
                    QMessageBox::Ok, self.data());
                QMessageBox& box = *boxOwner.get();
                box.setDetailedText(result.errors.join(QLatin1Char('\n')));
                box.exec();
                return;
            }

            ScopedChildWidget<QMessageBox> boxOwner(
                result.errors.isEmpty() ? QMessageBox::Information : QMessageBox::Warning,
                QStringLiteral("Import KiwiSDR Receivers"),
                QStringLiteral("Added %1 and updated %2 receiver(s) from %3.")
                    .arg(result.addedCount)
                    .arg(result.mergedCount)
                    .arg(QFileInfo(path).fileName()),
                QMessageBox::Ok, self.data());
            QMessageBox& box = *boxOwner.get();
            if (!result.errors.isEmpty()) {
                box.setInformativeText(
                    QStringLiteral("%1 row(s) could not be imported.")
                        .arg(result.errors.size()));
                box.setDetailedText(result.errors.join(QLatin1Char('\n')));
            }
            box.exec();
        });
        kiwiTransferRow->addWidget(kiwiImportBtn);

        auto* kiwiExportBtn = new QPushButton("Export...");
        kiwiExportBtn->setObjectName(QStringLiteral("kiwiExportButton"));
        kiwiExportBtn->setAccessibleName("Export KiwiSDR receivers");
        kiwiExportBtn->setAccessibleDescription(
            "Export the saved KiwiSDR receivers to a CSV file. Passwords "
            "are not included.");
        styleKiwiButton(kiwiExportBtn);
        connect(kiwiExportBtn, &QPushButton::clicked, this,
                [this, kiwiTransferDirectory, rememberKiwiTransferDirectory] {
            const QPointer<RadioSetupDialog> self(this);
            const QPointer<KiwiSdrManager> manager(m_kiwiSdrManager);
            const QString fileName = QStringLiteral("AetherSDR_KiwiSDR_Receivers_%1.csv")
                                         .arg(QDateTime::currentDateTime().toString(
                                             QStringLiteral("yyyyMMdd_HHmmss")));
            ScopedChildWidget<QFileDialog> dialogOwner(
                this, QStringLiteral("Export KiwiSDR Receivers"),
                QDir(kiwiTransferDirectory()).filePath(fileName),
                QStringLiteral("CSV Files (*.csv)"));
            QFileDialog& dialog = *dialogOwner.get();
            dialog.setAcceptMode(QFileDialog::AcceptSave);
            dialog.setDefaultSuffix(QStringLiteral("csv"));
            const int dialogResult = dialog.exec();
            if (!self || !manager || !dialogOwner || dialogResult != QDialog::Accepted
                || dialog.selectedFiles().isEmpty()) {
                return;
            }
            const QString path = dialog.selectedFiles().first();
            rememberKiwiTransferDirectory(path);

            const KiwiSdrCsvExportResult result = manager->exportToFile(path);
            if (!self || !manager) {
                return;
            }
            if (!result.ok()) {
                ScopedChildWidget<QMessageBox> boxOwner(
                    QMessageBox::Warning, QStringLiteral("Export KiwiSDR Receivers"),
                    result.error, QMessageBox::Ok, self.data());
                QMessageBox& box = *boxOwner.get();
                box.exec();
                return;
            }
            ScopedChildWidget<QMessageBox> boxOwner(
                QMessageBox::Information, QStringLiteral("Export KiwiSDR Receivers"),
                QStringLiteral("Exported %1 receiver(s) to %2. Passwords are not "
                               "included; re-enter them after importing elsewhere.")
                    .arg(result.exportedCount)
                    .arg(QFileInfo(path).fileName()),
                QMessageBox::Ok, self.data());
            QMessageBox& box = *boxOwner.get();
            box.exec();
        });
        kiwiTransferRow->addWidget(kiwiExportBtn);
        kiwiLayout->addLayout(kiwiTransferRow);
    }

    vbox->addStretch(1);
    (*refresh)();
    if (refreshKiwi && *refreshKiwi) {
        (*refreshKiwi)();
    }
    return page;
}

// APD tab (External Adaptive Pre-Distortion): per-TX-antenna sample port for
// APD adaptation. INTERNAL samples inside the radio; RX_A/RX_B/XVTA/XVTB take a
// coupled feedback signal, needed with an external linear. Hidden until the
// radio reports `apd configurable=1` (FLEX-8x00 on SmartSDR 4.2.18+).

QWidget* RadioSetupDialog::buildApdTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Model header — matches other tabs (e.g. Filters, TX)
    {
        auto* hdr = new QHBoxLayout;
        hdr->addStretch(1);
        auto* modelLbl = new QLabel(m_model->model());
        setTone(modelLbl, Tone::Title);
        hdr->addWidget(modelLbl);
        vbox->addLayout(hdr);
    }

    auto& tx = m_model->transmitModel();

    // External Sampler group — 2-column grid: ANT1/XVTA on the top row,
    // ANT2/XVTB on the bottom row, then the Reset button on its own row.
    {
        auto* group = new QGroupBox("External Sampler (per TX ANT)");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(8);

        // Row, col-pair, antenna name → builds label + combo, hooks signals.
        auto buildRow = [&](int row, int colBase, const QString& ant) {
            auto* lbl = new QLabel(ant + ":");
            applyLabelStyle(lbl);
            grid->addWidget(lbl, row, colBase);

            auto* combo = new QComboBox;
            const auto s = tx.apdSampler(ant);
            combo->addItems(s.available);
            combo->setCurrentText(s.selected);
            AetherSDR::applyComboStyle(combo);
            grid->addWidget(combo, row, colBase + 1);

            m_apdSamplerCombos.insert(ant, combo);

            connect(combo, &QComboBox::currentTextChanged, this,
                    [this, ant](const QString& port) {
                if (port.isEmpty()) return;
                m_model->transmitModel().setApdSamplerPort(ant, port);
            });
        };

        buildRow(0, 0, "ANT1");
        buildRow(0, 2, "XVTA");
        buildRow(1, 0, "ANT2");
        buildRow(1, 2, "XVTB");

        // Equalizer Reset button (row 2) — clears all per-antenna training.
        auto* resetLbl = new QLabel("Equalizer Reset:");
        applyLabelStyle(resetLbl);
        grid->addWidget(resetLbl, 2, 0);

        auto* resetBtn = new QPushButton("Reset");
        connect(resetBtn, &QPushButton::clicked, this, [this] {
            m_model->transmitModel().resetApdEqualizer();
        });
        grid->addWidget(resetBtn, 2, 1, Qt::AlignLeft);

        vbox->addWidget(group);
    }

    // Live updates: when sampler status arrives later (e.g. ANT changes,
    // first-connection populate), refresh the relevant combo's options
    // without firing change-signals back to the radio.
    connect(&tx, &TransmitModel::apdSamplerChanged, this,
            &RadioSetupDialog::refreshApdSamplerCombo);

    vbox->addStretch(1);
    return page;
}

void RadioSetupDialog::refreshApdSamplerCombo(const QString& txAnt)
{
    auto* combo = m_apdSamplerCombos.value(txAnt);
    if (!combo) return;

    const auto s = m_model->transmitModel().apdSampler(txAnt);
    QSignalBlocker b(combo);
    combo->clear();
    combo->addItems(s.available);
    combo->setCurrentText(s.selected);
}

// ── USB Cables tab ───────────────────────────────────────────────────────────

QWidget* RadioSetupDialog::buildUsbCablesTab()
{
    auto* page = new QWidget;
    auto* hbox = new QHBoxLayout(page);
    hbox->setSpacing(6);

    auto* cableModel = &m_model->usbCableModel();

    // Style constants
    // Token template (applied via applyStyleSheet) so the indicator is visible
    // in dark mode, matching the other checkboxes in this dialog. #c8d8e8 is
    // exactly {{color.text.primary}}, so the text colour is unchanged (#4012).

    // ── Left: cable list ────────────────────────────────────────────────
    auto* listGroup = new QGroupBox("Cables");
    listGroup->setFixedWidth(180);
    auto* listLayout = new QVBoxLayout(listGroup);

    auto* cableList = new QListWidget;
    listLayout->addWidget(cableList);
    hbox->addWidget(listGroup);

    // ── Right: stacked property panels ──────────────────────────────────
    auto* stack = new QStackedWidget;

    // Page 0: No cable selected
    {
        auto* empty = new QWidget;
        auto* emptyLayout = new QVBoxLayout(empty);
        auto* lbl = new QLabel("No USB cables detected.\n\nPlug a USB-serial adapter\n"
                               "into the radio's rear USB port.");
        lbl->setAlignment(Qt::AlignCenter);
        setTone(lbl, Tone::Hint);
        emptyLayout->addWidget(lbl);
        stack->addWidget(empty);  // index 0
    }

    // Helper: create source combo (shared across CAT, BCD, Bit)
    auto makeSourceCombo = []() {
        // GuardedComboBox: this combo lives in the scroll-wrapped USB Cables
        // tab and sends a destructive `source=` set on change, so a stray
        // wheel-scroll must not silently re-route a live cable's source.
        auto* combo = new GuardedComboBox;
        combo->addItems({"None", "TX Pan", "TX Slice", "Active Slice",
                         "TX Ant", "RX Ant", "Ordinal Slice"});
        combo->setAccessibleName("Cable source");
        combo->setAccessibleDescription(
            "Signal source routed to this cable");
        return combo;
    };
    // Map source display name → protocol value
    auto sourceToProto = [](const QString& display) -> QString {
        if (display == "TX Pan")        return "tx_pan";
        if (display == "TX Slice")      return "tx_slice";
        if (display == "Active Slice")  return "active_slice";
        if (display == "TX Ant")        return "tx_ant";
        if (display == "RX Ant")        return "rx_ant";
        if (display == "Ordinal Slice") return "ordinal_slice";
        return "None";
    };
    auto protoToSource = [](const QString& proto) -> int {
        if (proto == "tx_pan")        return 1;
        if (proto == "tx_slice")      return 2;
        if (proto == "active_slice")  return 3;
        if (proto == "tx_ant")        return 4;
        if (proto == "rx_ant")        return 5;
        if (proto == "ordinal_slice") return 6;
        return 0;  // None
    };

    struct SerialWidgets {
        QGroupBox* group{nullptr};
        QComboBox* speed{nullptr};
        QComboBox* data{nullptr};
        QComboBox* parity{nullptr};
        QComboBox* stop{nullptr};
        QComboBox* flow{nullptr};
    };

    // Cable Type combo: values match FlexLib's UsbCableType enum, listed here
    // in enum order as the single source of truth for the combo's item
    // order, the proto string sent on the wire, and the index recovered when
    // a status arrives — collapses what used to be three hand-synced lists
    // (combo item order, typeIndexToProto, protoToTypeIndex) into one.
    // "Invalid" is the unconfigured sentinel, not a menu entry. Selecting
    // BCD sends bare type=bcd; the existing bcdTypeCombo refines the specific
    // bcd/vbcd/bcd_vbcd sub-type afterward (matches FlexLib's own default-
    // then-refine behavior for this exact transition).
    struct CableTypeEntry { QString proto; QString label; };
    static const QVector<CableTypeEntry> kCableTypes = {
        {"cat",         "CAT"},
        {"bit",         "Bit"},
        {"bcd",         "BCD"},
        {"ldpa",        "LDPA"},
        {"passthrough", "Passthrough"},
    };
    // Helper: create Cable Type combo (shared across all pages). Guarded
    // against accidental wheel-scroll retyping a live cable (#570/#676-style
    // hazard — this combo's value change sends a destructive command).
    auto makeTypeCombo = []() {
        auto* combo = new GuardedComboBox;
        for (const auto& entry : kCableTypes) {
            combo->addItem(entry.label);
        }
        combo->setAccessibleName("Cable Type");
        combo->setAccessibleDescription(
            "Selects this cable's protocol type: CAT, Bit, BCD, LDPA, or Passthrough");
        return combo;
    };
    auto typeIndexToProto = [](int idx) -> QString {
        if (idx >= 0 && idx < kCableTypes.size()) {
            return kCableTypes[idx].proto;
        }
        return kCableTypes[0].proto;  // default: CAT
    };
    auto protoToTypeIndex = [](const QString& proto) -> int {
        // bcd/vbcd/bcd_vbcd are one family in FlexLib's UsbCableType — collapse
        // before lookup so any of the three sub-types selects the BCD entry.
        const QString family = (proto == "vbcd" || proto == "bcd_vbcd") ? "bcd" : proto;
        for (int i = 0; i < kCableTypes.size(); ++i) {
            if (kCableTypes[i].proto == family) {
                return i;
            }
        }
        return -1;  // invalid / unrecognized — leave the combo unset
    };

    // Helper: build the "Cable Settings" header group (Name/[Enabled]/Status/
    // Type) shared by all six cable pages. `includeEnabled` is false only for
    // the Unconfigured page, which has no enable state to toggle yet.
    struct CableHeaderWidgets {
        QGroupBox* group{nullptr};
        QLineEdit* nameEdit{nullptr};
        QCheckBox* enabledCheck{nullptr};  // nullptr when !includeEnabled
        QLabel*    statusLabel{nullptr};
        QComboBox* typeCombo{nullptr};
    };
    auto makeCableHeader = [makeTypeCombo](bool includeEnabled) -> CableHeaderWidgets {
        auto* group = new QGroupBox("Cable Settings");
        auto* hg = new QGridLayout(group);
        hg->setSpacing(4);

        int row = 0;
        hg->addWidget(new QLabel("Name:"), row, 0);
        auto* nameEdit = new QLineEdit;
        nameEdit->setAccessibleName("Cable name");
        hg->addWidget(nameEdit, row, 1);
        ++row;

        QCheckBox* enabledCheck = nullptr;
        if (includeEnabled) {
            enabledCheck = new QCheckBox("Enabled");
            hg->addWidget(enabledCheck, row, 0, 1, 2);
            ++row;
        }

        auto* statusLabel = new QLabel("Unplugged");
        setTone(statusLabel, Tone::Hint);
        hg->addWidget(new QLabel("Status:"), row, 0);
        hg->addWidget(statusLabel, row, 1);
        ++row;

        hg->addWidget(new QLabel("Type:"), row, 0);
        auto* typeCombo = makeTypeCombo();
        hg->addWidget(typeCombo, row, 1);

        return CableHeaderWidgets{group, nameEdit, enabledCheck, statusLabel, typeCombo};
    };

    // Serial of a cable being retyped, so cableAdded (see "Wire model signals"
    // below) reselects it after a type change rebuilds the entry
    // (UsbCableModel::applyStatus). Also cleared by a deferred 0 ms timer on
    // cableRemoved (a genuine retype's cableAdded runs first, synchronously) and
    // by a bounded timeout if the radio never echoes.
    auto pendingTypeChangeSn = std::make_shared<QString>();
    // Generation counter so the bounded timeout can't clear a *newer* pending.
    // Each send bumps the generation; the timer captures its own generation and
    // only clears if it's still current. Without this, retyping the same serial
    // twice within 5s would let the first timer clear the second retype's
    // pending (QTimer::singleShot is fire-and-forget and can't be cancelled),
    // reintroducing the stuck-panel bug on exactly the lossy radios the timeout
    // exists to protect.
    auto pendingTypeChangeGen = std::make_shared<quint64>(0);
    auto sendCableType = [cableModel, cableList, pendingTypeChangeSn,
                          pendingTypeChangeGen](const QString& proto) {
        auto* item = cableList->currentItem();
        if (!item) {
            return;
        }
        const QString sn = item->data(Qt::UserRole).toString();
        *pendingTypeChangeSn = sn;
        const quint64 gen = ++(*pendingTypeChangeGen);
        cableModel->sendSet(sn, "type", proto);
        QTimer::singleShot(5000, [pendingTypeChangeSn, pendingTypeChangeGen, gen]() {
            if (*pendingTypeChangeGen == gen) {
                pendingTypeChangeSn->clear();
            }
        });
    };
    // Wires a Cable Type combo's index changes to sendCableType — collapses
    // what used to be six copy-pasted connect() blocks into one call site.
    auto wireTypeCombo = [this, sendCableType, typeIndexToProto](QComboBox* combo) {
        connect(combo, &QComboBox::currentIndexChanged, this,
                [sendCableType, typeIndexToProto](int idx) {
            if (idx >= 0) {
                sendCableType(typeIndexToProto(idx));
            }
        });
    };

    // Helper: serial parameter group (shared by CAT and Passthrough)
    auto makeSerialGroup = [](const QString& title) -> SerialWidgets {
        auto* group = new QGroupBox(title);
        auto* grid = new QGridLayout(group);
        grid->setSpacing(4);

        auto* speedCombo = new QComboBox;
        for (int s : {300,600,1200,2400,4800,9600,14400,19200,38400,57600,115200,230400,460800,921600})
            speedCombo->addItem(QString::number(s));
        speedCombo->setCurrentText("9600");
        grid->addWidget(new QLabel("Speed:"), 0, 0);
        grid->addWidget(speedCombo, 0, 1);

        auto* dataCombo = new QComboBox;
        dataCombo->addItems({"7", "8"});
        dataCombo->setCurrentText("8");
        grid->addWidget(new QLabel("Data Bits:"), 1, 0);
        grid->addWidget(dataCombo, 1, 1);

        auto* parityCombo = new QComboBox;
        parityCombo->addItems({"none", "odd", "even", "mark", "space"});
        grid->addWidget(new QLabel("Parity:"), 2, 0);
        grid->addWidget(parityCombo, 2, 1);

        auto* stopCombo = new QComboBox;
        stopCombo->addItems({"1", "2"});
        grid->addWidget(new QLabel("Stop Bits:"), 3, 0);
        grid->addWidget(stopCombo, 3, 1);

        auto* flowCombo = new QComboBox;
        flowCombo->addItems({"none", "rts_cts", "dtr_dsr", "xon_xoff"});
        grid->addWidget(new QLabel("Flow:"), 4, 0);
        grid->addWidget(flowCombo, 4, 1);

        return SerialWidgets{group, speedCombo, dataCombo, parityCombo, stopCombo, flowCombo};
    };

    // Page 1: CAT cable
    QWidget* catPage;
    QLineEdit* catNameEdit;
    QCheckBox* catEnabledCheck;
    QLabel*    catStatusLabel;
    QComboBox* catSourceCombo;
    QCheckBox* catAutoReportCheck;
    SerialWidgets catSerialWidgets;
    QComboBox* catTypeCombo;
    {
        catPage = new QWidget;
        auto* vbox = new QVBoxLayout(catPage);
        vbox->setSpacing(6);

        // Common header
        auto catHeader = makeCableHeader(/*includeEnabled=*/true);
        catNameEdit = catHeader.nameEdit;
        catEnabledCheck = catHeader.enabledCheck;
        catStatusLabel = catHeader.statusLabel;
        catTypeCombo = catHeader.typeCombo;
        vbox->addWidget(catHeader.group);

        // Serial params
        catSerialWidgets = makeSerialGroup("Serial Parameters");
        vbox->addWidget(catSerialWidgets.group);

        // CAT source
        auto* srcGroup = new QGroupBox("CAT Source");
        auto* sg = new QGridLayout(srcGroup);
        sg->setSpacing(4);
        sg->addWidget(new QLabel("Source:"), 0, 0);
        catSourceCombo = makeSourceCombo();
        sg->addWidget(catSourceCombo, 0, 1);
        catAutoReportCheck = new QCheckBox("Auto Report");
        sg->addWidget(catAutoReportCheck, 1, 0, 1, 2);
        vbox->addWidget(srcGroup);

        vbox->addStretch();
        stack->addWidget(catPage);  // index 1
    }

    // Page 2: BCD cable
    QWidget* bcdPage;
    QLineEdit* bcdNameEdit;
    QCheckBox* bcdEnabledCheck;
    QLabel*    bcdStatusLabel;
    QComboBox* bcdSourceCombo;
    QComboBox* bcdTypeCombo;
    QComboBox* bcdPolarityCombo;
    QComboBox* bcdCableTypeCombo;
    {
        bcdPage = new QWidget;
        auto* vbox = new QVBoxLayout(bcdPage);
        vbox->setSpacing(6);

        auto bcdHeader = makeCableHeader(/*includeEnabled=*/true);
        bcdNameEdit = bcdHeader.nameEdit;
        bcdEnabledCheck = bcdHeader.enabledCheck;
        bcdStatusLabel = bcdHeader.statusLabel;
        bcdCableTypeCombo = bcdHeader.typeCombo;
        vbox->addWidget(bcdHeader.group);

        auto* bcdGroup = new QGroupBox("BCD Settings");
        auto* bg = new QGridLayout(bcdGroup);
        bg->setSpacing(4);
        bg->addWidget(new QLabel("BCD Type:"), 0, 0);
        // GuardedComboBox: scroll-wrapped tab + destructive `type=` on change.
        // NB this sub-type combo sends `type=` via sendBcdProp, which does NOT
        // arm pendingTypeChangeSn. That is correct only because
        // UsbCableModel::normalizeTypeFamily() collapses bcd/vbcd/bcd_vbcd to
        // one family, so a sub-type change updates in place (cableChanged) and
        // never triggers the remove+recreate path that would need a reselect.
        // If that invariant ever changes, route this through sendCableType.
        bcdTypeCombo = new GuardedComboBox;
        bcdTypeCombo->addItems({"HF (bcd)", "VHF (vbcd)", "HF+VHF (bcd_vbcd)"});
        bcdTypeCombo->setAccessibleName("BCD sub-type");
        bg->addWidget(bcdTypeCombo, 0, 1);
        bg->addWidget(new QLabel("Polarity:"), 1, 0);
        bcdPolarityCombo = new GuardedComboBox;  // destructive polarity= on change
        bcdPolarityCombo->addItems({"Active High", "Active Low"});
        bcdPolarityCombo->setAccessibleName("BCD polarity");
        bg->addWidget(bcdPolarityCombo, 1, 1);
        bg->addWidget(new QLabel("Source:"), 2, 0);
        bcdSourceCombo = makeSourceCombo();
        bg->addWidget(bcdSourceCombo, 2, 1);
        vbox->addWidget(bcdGroup);

        vbox->addStretch();
        stack->addWidget(bcdPage);  // index 2
    }

    // Page 3: Bit cable
    // Master-detail sub-view (Bit 0-7 list + one detail form), mirroring the
    // outer cable-list/stack pattern above — a flat 8-row grid can't fit
    // PTT-dependent/PTT-delay/TX-delay/freq-range/antenna-slice fields
    // without becoming unreadably cramped at this panel's width (#3607).
    QWidget* bitPage;
    QLineEdit* bitNameEdit;
    QCheckBox* bitEnabledCheck;
    QLabel*    bitStatusLabel;
    QListWidget* bitIndexList;
    struct BitWidgets {
        QCheckBox* enabled{nullptr};
        QComboBox* source{nullptr};
        QComboBox* sourceDetail{nullptr};
        QLabel*    sourceDetailLabel{nullptr};
        QComboBox* output{nullptr};
        QLineEdit* band{nullptr};
        QLabel*    bandLabel{nullptr};
        QLineEdit* lowFreq{nullptr};
        QLabel*    lowFreqLabel{nullptr};
        QLineEdit* highFreq{nullptr};
        QLabel*    highFreqLabel{nullptr};
        QComboBox* polarity{nullptr};
        QCheckBox* pttDependent{nullptr};
        QSpinBox*  pttDelay{nullptr};
        QSpinBox*  txDelay{nullptr};
    };
    BitWidgets bitWidgets;
    auto currentBit = std::make_shared<int>(0);
    QComboBox* bitTypeCombo;
    {
        bitPage = new QWidget;
        auto* vbox = new QVBoxLayout(bitPage);
        vbox->setSpacing(6);

        auto bitHeader = makeCableHeader(/*includeEnabled=*/true);
        bitNameEdit = bitHeader.nameEdit;
        bitEnabledCheck = bitHeader.enabledCheck;
        bitStatusLabel = bitHeader.statusLabel;
        bitTypeCombo = bitHeader.typeCombo;
        vbox->addWidget(bitHeader.group);

        auto* bitSplit = new QHBoxLayout;

        auto* bitListGroup = new QGroupBox("Bits");
        bitListGroup->setFixedWidth(90);
        auto* bitListVbox = new QVBoxLayout(bitListGroup);
        bitIndexList = new QListWidget;
        for (int b = 0; b < 8; ++b)
            bitIndexList->addItem(QString("Bit %1").arg(b));
        bitListVbox->addWidget(bitIndexList);
        bitSplit->addWidget(bitListGroup);

        auto* bitDetailGroup = new QGroupBox("Bit Settings");
        auto* bd = new QGridLayout(bitDetailGroup);
        bd->setSpacing(6);

        int row = 0;
        auto addRow = [&](const QString& label, QWidget* w) -> QLabel* {
            auto* lbl = new QLabel(label);
            bd->addWidget(lbl, row, 0);
            bd->addWidget(w, row, 1);
            ++row;
            return lbl;
        };

        bitWidgets.enabled = new QCheckBox;
        addRow("Enabled:", bitWidgets.enabled);

        bitWidgets.source = makeSourceCombo();
        addRow("Source:", bitWidgets.source);

        bitWidgets.sourceDetail = new QComboBox;
        bitWidgets.sourceDetail->setEditable(true);
        bitWidgets.sourceDetailLabel = addRow("Antenna/Slice:", bitWidgets.sourceDetail);

        bitWidgets.output = new QComboBox;
        bitWidgets.output->addItems({"band", "freq_range"});
        addRow("Output:", bitWidgets.output);

        bitWidgets.band = new QLineEdit;
        bitWidgets.band->setPlaceholderText("e.g. 20");
        bitWidgets.bandLabel = addRow("Band:", bitWidgets.band);

        bitWidgets.lowFreq = new QLineEdit;
        bitWidgets.lowFreq->setPlaceholderText("e.g. 0.100");
        bitWidgets.lowFreqLabel = addRow("Low Freq (MHz):", bitWidgets.lowFreq);

        bitWidgets.highFreq = new QLineEdit;
        bitWidgets.highFreq->setPlaceholderText("e.g. 54.000");
        bitWidgets.highFreqLabel = addRow("High Freq (MHz):", bitWidgets.highFreq);

        bitWidgets.polarity = new QComboBox;
        bitWidgets.polarity->addItems({"High", "Low"});
        addRow("Polarity:", bitWidgets.polarity);

        bitWidgets.pttDependent = new QCheckBox;
        addRow("PTT Dependent:", bitWidgets.pttDependent);

        bitWidgets.pttDelay = new QSpinBox;
        bitWidgets.pttDelay->setRange(0, 10000);
        bitWidgets.pttDelay->setSingleStep(5);
        bitWidgets.pttDelay->setSuffix(" ms");
        addRow("PTT Delay:", bitWidgets.pttDelay);

        bitWidgets.txDelay = new QSpinBox;
        bitWidgets.txDelay->setRange(0, 10000);
        bitWidgets.txDelay->setSingleStep(5);
        bitWidgets.txDelay->setSuffix(" ms");
        addRow("TX Delay:", bitWidgets.txDelay);

        bd->setRowStretch(row, 1);
        bitSplit->addWidget(bitDetailGroup, 1);

        vbox->addLayout(bitSplit);
        vbox->addStretch();
        stack->addWidget(bitPage);  // index 3
    }

    // Bit detail refresh helpers — output mode (band vs freq-range) and
    // source (antenna/slice sub-selector) each drive which sibling fields
    // are visible; kept as named lambdas so both the interactive wiring
    // below and the model-driven repopulation in showCableProps can call
    // the exact same logic.
    auto refreshBitOutputVisibility = [bitWidgets]() {
        const bool isFreqRange = (bitWidgets.output->currentText() == "freq_range");
        bitWidgets.band->setVisible(!isFreqRange);
        bitWidgets.bandLabel->setVisible(!isFreqRange);
        bitWidgets.lowFreq->setVisible(isFreqRange);
        bitWidgets.lowFreqLabel->setVisible(isFreqRange);
        bitWidgets.highFreq->setVisible(isFreqRange);
        bitWidgets.highFreqLabel->setVisible(isFreqRange);
    };
    auto refreshBitSourceDetail = [bitWidgets, sourceToProto, this]() {
        const QString proto = sourceToProto(bitWidgets.source->currentText());
        const bool needsDetail = (proto == "tx_ant" || proto == "rx_ant" || proto == "ordinal_slice");
        bitWidgets.sourceDetail->setVisible(needsDetail);
        bitWidgets.sourceDetailLabel->setVisible(needsDetail);
        if (!needsDetail) return;
        QSignalBlocker blocker(bitWidgets.sourceDetail);
        bitWidgets.sourceDetail->clear();
        if (proto == "ordinal_slice") {
            for (int i = 0; i < m_model->maxSlices(); ++i)
                bitWidgets.sourceDetail->addItem(QString::number(i));
        } else {
            bitWidgets.sourceDetail->addItems(m_model->knownAntennaTokens());
        }
    };
    refreshBitOutputVisibility();
    refreshBitSourceDetail();

    auto refreshBitDetail = [=]() {
        auto* item = cableList->currentItem();
        if (!item) return;
        const QString sn = item->data(Qt::UserRole).toString();
        if (!cableModel->cables().contains(sn)) return;
        const auto& cable = cableModel->cables()[sn];
        if (cable.type != "bit") return;
        const int b = qBound(0, *currentBit, 7);
        const auto& bit = cable.bits[b];

        QSignalBlocker blk1(bitWidgets.enabled), blk2(bitWidgets.source),
                       blk3(bitWidgets.sourceDetail), blk4(bitWidgets.output),
                       blk5(bitWidgets.band), blk6(bitWidgets.lowFreq),
                       blk7(bitWidgets.highFreq), blk8(bitWidgets.polarity),
                       blk9(bitWidgets.pttDependent), blk10(bitWidgets.pttDelay),
                       blk11(bitWidgets.txDelay);

        bitWidgets.enabled->setChecked(bit.enabled);
        bitWidgets.source->setCurrentIndex(protoToSource(bit.source));
        bitWidgets.output->setCurrentText(bit.output.isEmpty() ? "band" : bit.output);
        bitWidgets.band->setText(bit.band);
        bitWidgets.lowFreq->setText(QString::number(bit.lowFreqMhz, 'f', 3));
        bitWidgets.highFreq->setText(QString::number(bit.highFreqMhz, 'f', 3));
        bitWidgets.polarity->setCurrentIndex(bit.activeHigh ? 0 : 1);
        bitWidgets.pttDependent->setChecked(bit.pttDependent);
        bitWidgets.pttDelay->setValue(bit.pttDelayMs);
        bitWidgets.txDelay->setValue(bit.txDelayMs);

        refreshBitOutputVisibility();
        refreshBitSourceDetail();
        QString detailValue;
        if (bit.source == "tx_ant")           detailValue = bit.sourceTxAnt;
        else if (bit.source == "rx_ant")      detailValue = bit.sourceRxAnt;
        else if (bit.source == "ordinal_slice") detailValue = bit.sourceSlice;
        bitWidgets.sourceDetail->setCurrentText(detailValue);
    };

    // Page 4: Passthrough cable
    QWidget* ptPage;
    QLineEdit* ptNameEdit;
    QCheckBox* ptEnabledCheck;
    QLabel*    ptStatusLabel;
    SerialWidgets ptSerialWidgets;
    QComboBox* ptTypeCombo;
    {
        ptPage = new QWidget;
        auto* vbox = new QVBoxLayout(ptPage);
        vbox->setSpacing(6);

        auto ptHeader = makeCableHeader(/*includeEnabled=*/true);
        ptNameEdit = ptHeader.nameEdit;
        ptEnabledCheck = ptHeader.enabledCheck;
        ptStatusLabel = ptHeader.statusLabel;
        ptTypeCombo = ptHeader.typeCombo;
        vbox->addWidget(ptHeader.group);

        ptSerialWidgets = makeSerialGroup("Serial Parameters");
        vbox->addWidget(ptSerialWidgets.group);

        vbox->addStretch();
        stack->addWidget(ptPage);  // index 4
    }

    // Page 5: LDPA cable
    QWidget* ldpaPage;
    QLineEdit* ldpaNameEdit;
    QCheckBox* ldpaEnabledCheck;
    QLabel*    ldpaStatusLabel;
    QComboBox* ldpaTypeCombo;
    QComboBox* ldpaBandCombo;
    QCheckBox* ldpaPreampCheck;
    QComboBox* ldpaSourceCombo;
    {
        ldpaPage = new QWidget;
        auto* vbox = new QVBoxLayout(ldpaPage);
        vbox->setSpacing(6);

        auto ldpaHeader = makeCableHeader(/*includeEnabled=*/true);
        ldpaNameEdit = ldpaHeader.nameEdit;
        ldpaEnabledCheck = ldpaHeader.enabledCheck;
        ldpaStatusLabel = ldpaHeader.statusLabel;
        ldpaTypeCombo = ldpaHeader.typeCombo;
        vbox->addWidget(ldpaHeader.group);

        auto* ldpaGroup = new QGroupBox("LDPA Settings");
        auto* lg = new QGridLayout(ldpaGroup);
        lg->setSpacing(4);
        lg->addWidget(new QLabel("Band:"), 0, 0);
        // GuardedComboBox: scroll-wrapped tab + destructive `band=` on change.
        ldpaBandCombo = new GuardedComboBox;
        ldpaBandCombo->addItems({"2m", "4m"});
        ldpaBandCombo->setAccessibleName("LDPA band");
        ldpaBandCombo->setAccessibleDescription("LDPA amplifier band (2m or 4m)");
        lg->addWidget(ldpaBandCombo, 0, 1);
        ldpaPreampCheck = new QCheckBox("Preamp");
        lg->addWidget(ldpaPreampCheck, 1, 0, 1, 2);
        lg->addWidget(new QLabel("Source:"), 2, 0);
        ldpaSourceCombo = makeSourceCombo();
        lg->addWidget(ldpaSourceCombo, 2, 1);
        vbox->addWidget(ldpaGroup);

        vbox->addStretch();
        stack->addWidget(ldpaPage);  // index 5
    }

    // Page 6: Unconfigured cable (type is "invalid" or otherwise unrecognized —
    // a real, present cable that hasn't been assigned a type yet, distinct
    // from "nothing plugged in" at index 0)
    QWidget* unconfiguredPage;
    QLineEdit* unconfiguredNameEdit;
    QLabel*    unconfiguredStatusLabel;
    QComboBox* unconfiguredTypeCombo;
    QPushButton* unconfiguredRemoveBtn;
    {
        unconfiguredPage = new QWidget;
        auto* vbox = new QVBoxLayout(unconfiguredPage);
        vbox->setSpacing(6);

        auto unconfiguredHeader = makeCableHeader(/*includeEnabled=*/false);
        unconfiguredNameEdit = unconfiguredHeader.nameEdit;
        unconfiguredStatusLabel = unconfiguredHeader.statusLabel;
        unconfiguredTypeCombo = unconfiguredHeader.typeCombo;
        unconfiguredTypeCombo->setCurrentIndex(-1);  // force a real choice, no default
        vbox->addWidget(unconfiguredHeader.group);

        auto* note = new QLabel("Select a cable type to configure this device.");
        setTone(note, Tone::Hint);
        note->setWordWrap(true);
        vbox->addWidget(note);

        unconfiguredRemoveBtn = new QPushButton("Remove This Cable");
        unconfiguredRemoveBtn->setAutoDefault(false);
        vbox->addWidget(unconfiguredRemoveBtn);

        vbox->addStretch();
        stack->addWidget(unconfiguredPage);  // index 6
    }

    hbox->addWidget(stack, 1);

    // ── Populate cable list from model ──────────────────────────────────
    // A cable's color says whether it is enabled and plugged in.
    auto colorItem = [cableModel](QListWidgetItem* item) {
        const auto& cables = cableModel->cables();
        const auto it = cables.constFind(item->data(Qt::UserRole).toString());
        if (it == cables.constEnd()) {
            return;
        }
        const auto& theme = ThemeManager::instance();
        if (it->enabled && it->present)
            item->setForeground(theme.color(QStringLiteral("color.accent.success")));
        else if (it->enabled)
            item->setForeground(theme.color(QStringLiteral("color.accent.warning")));
        else
            item->setForeground(theme.color(QStringLiteral("color.canon.muted")));
    };
    auto refreshList = [cableList, cableModel, colorItem]() {
        QString prevSn;
        if (cableList->currentItem())
            prevSn = cableList->currentItem()->data(Qt::UserRole).toString();
        cableList->clear();
        for (auto it = cableModel->cables().begin(); it != cableModel->cables().end(); ++it) {
            const auto& cable = it.value();
            QString label = cable.name.isEmpty() ? cable.serialNumber : cable.name;
            label += QString(" [%1]").arg(cable.type.toUpper());
            if (!cable.present)
                label += " (unplugged)";
            auto* item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, cable.serialNumber);
            colorItem(item);
            cableList->addItem(item);
            if (cable.serialNumber == prevSn)
                cableList->setCurrentItem(item);
        }
    };

    // ── Select cable → show properties ──────────────────────────────────
    auto showCableProps = [=](const QString& sn) {
        if (sn.isEmpty() || !cableModel->cables().contains(sn)) {
            stack->setCurrentIndex(0);
            return;
        }
        const auto& cable = cableModel->cables()[sn];
        const QString& t = cable.type;

        if (t == "cat") {
            stack->setCurrentIndex(1);
            QSignalBlocker b1(catNameEdit), b2(catEnabledCheck), b3(catSourceCombo),
                           b4(catAutoReportCheck), b5(catTypeCombo);
            QSignalBlocker b6(catSerialWidgets.speed), b7(catSerialWidgets.data),
                           b8(catSerialWidgets.parity), b9(catSerialWidgets.stop),
                           b10(catSerialWidgets.flow);
            catNameEdit->setText(cable.name);
            catEnabledCheck->setChecked(cable.enabled);
            catStatusLabel->setText(cable.present ? "Plugged In" : "Unplugged");
            setTone(catStatusLabel, cable.present ? Tone::Ok : Tone::Hint);
            catSourceCombo->setCurrentIndex(protoToSource(cable.source));
            catAutoReportCheck->setChecked(cable.autoReport);
            catTypeCombo->setCurrentIndex(protoToTypeIndex(t));

            catSerialWidgets.speed->setCurrentText(QString::number(cable.speed));
            catSerialWidgets.data->setCurrentText(QString::number(cable.dataBits));
            catSerialWidgets.parity->setCurrentText(cable.parity);
            catSerialWidgets.stop->setCurrentText(QString::number(cable.stopBits));
            catSerialWidgets.flow->setCurrentText(cable.flowControl);
        } else if (t == "bcd" || t == "vbcd" || t == "bcd_vbcd") {
            stack->setCurrentIndex(2);
            QSignalBlocker b1(bcdNameEdit), b2(bcdEnabledCheck), b3(bcdSourceCombo),
                           b4(bcdTypeCombo), b5(bcdPolarityCombo), b6(bcdCableTypeCombo);
            bcdNameEdit->setText(cable.name);
            bcdEnabledCheck->setChecked(cable.enabled);
            bcdStatusLabel->setText(cable.present ? "Plugged In" : "Unplugged");
            setTone(bcdStatusLabel, cable.present ? Tone::Ok : Tone::Hint);
            bcdSourceCombo->setCurrentIndex(protoToSource(cable.source));
            if (t == "vbcd") bcdTypeCombo->setCurrentIndex(1);
            else if (t == "bcd_vbcd") bcdTypeCombo->setCurrentIndex(2);
            else bcdTypeCombo->setCurrentIndex(0);
            bcdPolarityCombo->setCurrentIndex(cable.activeHigh ? 0 : 1);
            bcdCableTypeCombo->setCurrentIndex(protoToTypeIndex(t));
        } else if (t == "bit") {
            stack->setCurrentIndex(3);
            QSignalBlocker b1(bitNameEdit), b2(bitEnabledCheck), b3(bitTypeCombo);
            bitNameEdit->setText(cable.name);
            bitEnabledCheck->setChecked(cable.enabled);
            bitStatusLabel->setText(cable.present ? "Plugged In" : "Unplugged");
            setTone(bitStatusLabel, cable.present ? Tone::Ok : Tone::Hint);
            bitTypeCombo->setCurrentIndex(protoToTypeIndex(t));
            if (bitIndexList->currentRow() < 0) {
                QSignalBlocker blk(bitIndexList);
                bitIndexList->setCurrentRow(0);
            }
            refreshBitDetail();
        } else if (t == "passthrough") {
            stack->setCurrentIndex(4);
            QSignalBlocker b1(ptNameEdit), b2(ptEnabledCheck), b3(ptTypeCombo);
            QSignalBlocker b4(ptSerialWidgets.speed), b5(ptSerialWidgets.data),
                           b6(ptSerialWidgets.parity), b7(ptSerialWidgets.stop),
                           b8(ptSerialWidgets.flow);
            ptNameEdit->setText(cable.name);
            ptEnabledCheck->setChecked(cable.enabled);
            ptStatusLabel->setText(cable.present ? "Plugged In" : "Unplugged");
            setTone(ptStatusLabel, cable.present ? Tone::Ok : Tone::Hint);
            ptTypeCombo->setCurrentIndex(protoToTypeIndex(t));

            ptSerialWidgets.speed->setCurrentText(QString::number(cable.speed));
            ptSerialWidgets.data->setCurrentText(QString::number(cable.dataBits));
            ptSerialWidgets.parity->setCurrentText(cable.parity);
            ptSerialWidgets.stop->setCurrentText(QString::number(cable.stopBits));
            ptSerialWidgets.flow->setCurrentText(cable.flowControl);
        } else if (t == "ldpa") {
            stack->setCurrentIndex(5);
            QSignalBlocker b1(ldpaNameEdit), b2(ldpaEnabledCheck), b3(ldpaTypeCombo),
                           b4(ldpaBandCombo), b5(ldpaPreampCheck), b6(ldpaSourceCombo);
            ldpaNameEdit->setText(cable.name);
            ldpaEnabledCheck->setChecked(cable.enabled);
            ldpaStatusLabel->setText(cable.present ? "Plugged In" : "Unplugged");
            setTone(ldpaStatusLabel, cable.present ? Tone::Ok : Tone::Hint);
            ldpaTypeCombo->setCurrentIndex(protoToTypeIndex(t));
            ldpaBandCombo->setCurrentIndex(cable.ldpaBand == "4" ? 1 : 0);
            ldpaPreampCheck->setChecked(cable.preamp);
            ldpaSourceCombo->setCurrentIndex(protoToSource(cable.source));
        } else {
            // "invalid" or any other unrecognized type: a real, present cable
            // that hasn't been assigned a type yet — distinct from "nothing
            // plugged in" (index 0), which is handled by the isEmpty()/
            // not-found guard above and by the cableRemoved handler.
            stack->setCurrentIndex(6);
            QSignalBlocker b1(unconfiguredNameEdit), b2(unconfiguredTypeCombo);
            unconfiguredNameEdit->setText(cable.name);
            unconfiguredStatusLabel->setText(cable.present ? "Plugged In" : "Unplugged");
            setTone(unconfiguredStatusLabel, cable.present ? Tone::Ok : Tone::Hint);
            unconfiguredTypeCombo->setCurrentIndex(-1);
        }
    };

    connect(cableList, &QListWidget::currentItemChanged, this,
            [showCableProps](QListWidgetItem* current, QListWidgetItem*) {
        if (current)
            showCableProps(current->data(Qt::UserRole).toString());
    });

    // ── Wire model signals ──────────────────────────────────────────────
    connect(cableModel, &UsbCableModel::cableAdded, this,
            [refreshList, cableList, showCableProps, pendingTypeChangeSn](const QString& sn) {
        refreshList();
        // A retype tears the old cable entry down and rebuilds it fresh
        // (UsbCableModel::applyStatus), which drops the list selection along
        // the way (see the cableRemoved handler below). Reselect and
        // repopulate the detail panel so a successful retype doesn't leave
        // it stuck on the empty-state page until manually re-clicked.
        if (*pendingTypeChangeSn == sn) {
            pendingTypeChangeSn->clear();
            for (int i = 0; i < cableList->count(); ++i) {
                if (cableList->item(i)->data(Qt::UserRole).toString() == sn) {
                    cableList->setCurrentItem(cableList->item(i));
                    break;
                }
            }
            showCableProps(sn);
        }
    });
    connect(cableModel, &UsbCableModel::cableRemoved, this,
            [refreshList, stack, pendingTypeChangeSn](const QString& sn) {
        refreshList();
        stack->setCurrentIndex(0);
        // Defer the clear to the next event-loop tick: a genuine retype's own
        // cableAdded for this serial fires synchronously right after this
        // handler returns (same applyStatus() call) and clears the pointer
        // itself first, so this is a no-op for that case. It only matters for
        // a standalone removal unrelated to any pending retype, preventing a
        // later unplug/replug's cableAdded from being mistaken for a retype
        // completion.
        if (*pendingTypeChangeSn == sn) {
            QTimer::singleShot(0, [pendingTypeChangeSn, sn]() {
                if (*pendingTypeChangeSn == sn) {
                    pendingTypeChangeSn->clear();
                }
            });
        }
    });
    connect(cableModel, &UsbCableModel::cableChanged, this,
            [refreshList, cableList, showCableProps](const QString& sn) {
        refreshList();
        if (cableList->currentItem() &&
            cableList->currentItem()->data(Qt::UserRole).toString() == sn)
            showCableProps(sn);
    });

    // ── Wire property edits → commands ──────────────────────────────────
    // CAT
    auto sendCatProp = [cableModel, cableList](const QString& key, const QString& val) {
        auto* item = cableList->currentItem();
        if (!item) return;
        cableModel->sendSet(item->data(Qt::UserRole).toString(), key, val);
    };
    connect(catNameEdit, &QLineEdit::editingFinished, this, [catNameEdit, sendCatProp]() {
        sendCatProp("name", QString(catNameEdit->text()).replace(' ', QChar(0x7F)));
    });
    connect(catEnabledCheck, &QCheckBox::toggled, this, [sendCatProp](bool on) {
        sendCatProp("enable", on ? "1" : "0");
    });
    connect(catSourceCombo, &QComboBox::currentTextChanged, this,
            [sendCatProp, sourceToProto](const QString& text) {
        sendCatProp("source", sourceToProto(text));
    });
    connect(catAutoReportCheck, &QCheckBox::toggled, this, [sendCatProp](bool on) {
        sendCatProp("auto_report", on ? "1" : "0");
    });
    wireTypeCombo(catTypeCombo);

    connect(catSerialWidgets.speed, &QComboBox::currentTextChanged, this, [sendCatProp](const QString& val) { sendCatProp("speed", val); });
    connect(catSerialWidgets.data, &QComboBox::currentTextChanged, this, [sendCatProp](const QString& val) { sendCatProp("data_bits", val); });
    connect(catSerialWidgets.parity, &QComboBox::currentTextChanged, this, [sendCatProp](const QString& val) { sendCatProp("parity", val); });
    connect(catSerialWidgets.stop, &QComboBox::currentTextChanged, this, [sendCatProp](const QString& val) { sendCatProp("stop_bits", val); });
    connect(catSerialWidgets.flow, &QComboBox::currentTextChanged, this, [sendCatProp](const QString& val) { sendCatProp("flow_control", val); });

    // BCD
    auto sendBcdProp = [cableModel, cableList](const QString& key, const QString& val) {
        auto* item = cableList->currentItem();
        if (!item) return;
        cableModel->sendSet(item->data(Qt::UserRole).toString(), key, val);
    };
    connect(bcdNameEdit, &QLineEdit::editingFinished, this, [bcdNameEdit, sendBcdProp]() {
        sendBcdProp("name", QString(bcdNameEdit->text()).replace(' ', QChar(0x7F)));
    });
    connect(bcdEnabledCheck, &QCheckBox::toggled, this, [sendBcdProp](bool on) {
        sendBcdProp("enable", on ? "1" : "0");
    });
    connect(bcdTypeCombo, &QComboBox::currentIndexChanged, this,
            [sendBcdProp](int idx) {
        static const char* types[] = {"bcd", "vbcd", "bcd_vbcd"};
        if (idx >= 0 && idx < 3) sendBcdProp("type", types[idx]);
    });
    connect(bcdPolarityCombo, &QComboBox::currentIndexChanged, this,
            [sendBcdProp](int idx) {
        sendBcdProp("polarity", idx == 0 ? "active_high" : "active_low");
    });
    connect(bcdSourceCombo, &QComboBox::currentTextChanged, this,
            [sendBcdProp, sourceToProto](const QString& text) {
        sendBcdProp("source", sourceToProto(text));
    });
    wireTypeCombo(bcdCableTypeCombo);

    // Bit cable header (whole-cable name/enable, cableModel->sendSet)
    auto sendBitProp = [cableModel, cableList](const QString& key, const QString& val) {
        auto* item = cableList->currentItem();
        if (!item) return;
        cableModel->sendSet(item->data(Qt::UserRole).toString(), key, val);
    };
    connect(bitNameEdit, &QLineEdit::editingFinished, this, [bitNameEdit, sendBitProp]() {
        sendBitProp("name", QString(bitNameEdit->text()).replace(' ', QChar(0x7F)));
    });
    connect(bitEnabledCheck, &QCheckBox::toggled, this, [sendBitProp](bool on) {
        sendBitProp("enable", on ? "1" : "0");
    });
    wireTypeCombo(bitTypeCombo);

    // Bit detail fields (per-bit, cableModel->sendSetBit against *currentBit)
    auto sendBitFieldProp = [cableModel, cableList, currentBit](const QString& key, const QString& val) {
        auto* item = cableList->currentItem();
        if (!item) return;
        cableModel->sendSetBit(item->data(Qt::UserRole).toString(), *currentBit, key, val);
    };
    connect(bitWidgets.enabled, &QCheckBox::toggled, this, [sendBitFieldProp](bool on) {
        sendBitFieldProp("enable", on ? "1" : "0");
    });
    connect(bitWidgets.source, &QComboBox::currentTextChanged, this,
            [sendBitFieldProp, sourceToProto, refreshBitSourceDetail](const QString& text) {
        sendBitFieldProp("source", sourceToProto(text));
        refreshBitSourceDetail();
    });
    connect(bitWidgets.sourceDetail, &QComboBox::currentTextChanged, this,
            [sendBitFieldProp, sourceToProto, bitWidgets](const QString& text) {
        if (text.isEmpty()) return;
        const QString proto = sourceToProto(bitWidgets.source->currentText());
        if (proto == "tx_ant") sendBitFieldProp("source_tx_ant", text);
        else if (proto == "rx_ant") sendBitFieldProp("source_rx_ant", text);
        else if (proto == "ordinal_slice") sendBitFieldProp("source_slice", text);
    });
    connect(bitWidgets.output, &QComboBox::currentTextChanged, this,
            [sendBitFieldProp, refreshBitOutputVisibility](const QString& text) {
        sendBitFieldProp("output", text);
        refreshBitOutputVisibility();
    });
    connect(bitWidgets.band, &QLineEdit::editingFinished, this, [sendBitFieldProp, bitWidgets]() {
        sendBitFieldProp("band", bitWidgets.band->text());
    });
    connect(bitWidgets.lowFreq, &QLineEdit::editingFinished, this, [sendBitFieldProp, bitWidgets]() {
        sendBitFieldProp("low_freq", QString::number(bitWidgets.lowFreq->text().toDouble(), 'f', 3));
    });
    connect(bitWidgets.highFreq, &QLineEdit::editingFinished, this, [sendBitFieldProp, bitWidgets]() {
        sendBitFieldProp("high_freq", QString::number(bitWidgets.highFreq->text().toDouble(), 'f', 3));
    });
    connect(bitWidgets.polarity, &QComboBox::currentTextChanged, this,
            [sendBitFieldProp](const QString& text) {
        sendBitFieldProp("polarity", text == "High" ? "active_high" : "active_low");
    });
    connect(bitWidgets.pttDependent, &QCheckBox::toggled, this, [sendBitFieldProp](bool on) {
        sendBitFieldProp("ptt_dependent", on ? "1" : "0");
    });
    connect(bitWidgets.pttDelay, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [sendBitFieldProp](int val) {
        sendBitFieldProp("ptt_delay", QString::number(val));
    });
    connect(bitWidgets.txDelay, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [sendBitFieldProp](int val) {
        sendBitFieldProp("tx_delay", QString::number(val));
    });
    connect(bitIndexList, &QListWidget::currentRowChanged, this,
            [currentBit, refreshBitDetail](int row) {
        if (row < 0) return;
        *currentBit = row;
        refreshBitDetail();
    });

    // Passthrough
    auto sendPtProp = [cableModel, cableList](const QString& key, const QString& val) {
        auto* item = cableList->currentItem();
        if (!item) return;
        cableModel->sendSet(item->data(Qt::UserRole).toString(), key, val);
    };
    connect(ptNameEdit, &QLineEdit::editingFinished, this, [ptNameEdit, sendPtProp]() {
        sendPtProp("name", QString(ptNameEdit->text()).replace(' ', QChar(0x7F)));
    });
    connect(ptEnabledCheck, &QCheckBox::toggled, this, [sendPtProp](bool on) {
        sendPtProp("enable", on ? "1" : "0");
    });
    wireTypeCombo(ptTypeCombo);

    // LDPA
    auto sendLdpaProp = [cableModel, cableList](const QString& key, const QString& val) {
        auto* item = cableList->currentItem();
        if (!item) return;
        cableModel->sendSet(item->data(Qt::UserRole).toString(), key, val);
    };
    connect(ldpaNameEdit, &QLineEdit::editingFinished, this, [ldpaNameEdit, sendLdpaProp]() {
        sendLdpaProp("name", QString(ldpaNameEdit->text()).replace(' ', QChar(0x7F)));
    });
    connect(ldpaEnabledCheck, &QCheckBox::toggled, this, [sendLdpaProp](bool on) {
        sendLdpaProp("enable", on ? "1" : "0");
    });
    wireTypeCombo(ldpaTypeCombo);
    connect(ldpaBandCombo, &QComboBox::currentIndexChanged, this,
            [sendLdpaProp](int idx) {
        sendLdpaProp("band", idx == 1 ? "4" : "2");
    });
    connect(ldpaPreampCheck, &QCheckBox::toggled, this, [sendLdpaProp](bool on) {
        sendLdpaProp("preamp", on ? "1" : "0");
    });
    connect(ldpaSourceCombo, &QComboBox::currentTextChanged, this,
            [sendLdpaProp, sourceToProto](const QString& text) {
        sendLdpaProp("source", sourceToProto(text));
    });

    // Unconfigured cable
    auto sendUnconfiguredProp = [cableModel, cableList](const QString& key, const QString& val) {
        auto* item = cableList->currentItem();
        if (!item) return;
        cableModel->sendSet(item->data(Qt::UserRole).toString(), key, val);
    };
    connect(unconfiguredNameEdit, &QLineEdit::editingFinished, this,
            [unconfiguredNameEdit, sendUnconfiguredProp]() {
        sendUnconfiguredProp("name", QString(unconfiguredNameEdit->text()).replace(' ', QChar(0x7F)));
    });
    wireTypeCombo(unconfiguredTypeCombo);
    connect(unconfiguredRemoveBtn, &QPushButton::clicked, this, [cableModel, cableList]() {
        auto* item = cableList->currentItem();
        if (!item) return;
        cableModel->sendRemove(item->data(Qt::UserRole).toString());
    });

    connect(ptSerialWidgets.speed, &QComboBox::currentTextChanged, this, [sendPtProp](const QString& val) { sendPtProp("speed", val); });
    connect(ptSerialWidgets.data, &QComboBox::currentTextChanged, this, [sendPtProp](const QString& val) { sendPtProp("data_bits", val); });
    connect(ptSerialWidgets.parity, &QComboBox::currentTextChanged, this, [sendPtProp](const QString& val) { sendPtProp("parity", val); });
    connect(ptSerialWidgets.stop, &QComboBox::currentTextChanged, this, [sendPtProp](const QString& val) { sendPtProp("stop_bits", val); });
    connect(ptSerialWidgets.flow, &QComboBox::currentTextChanged, this, [sendPtProp](const QString& val) { sendPtProp("flow_control", val); });

    // Initial populate
    refreshList();
    // The item colors are resolved tokens: re-resolve them on a theme switch,
    // leaving the list and the cable being edited as they are.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, cableList,
            [cableList, colorItem]() {
        for (int i = 0; i < cableList->count(); ++i)
            colorItem(cableList->item(i));
    });

    return page;
}

#ifdef HAVE_SERIALPORT
QWidget* RadioSetupDialog::buildSerialTab()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);
    vbox->setContentsMargins(8, 8, 8, 8);

    auto& settings = AppSettings::instance();

    // ── USB control surfaces (Ulanzi Dial, StreamDeck+) (#3257) ──────────
    // The Ulanzi Dial is on by default: every backend detects the dial before
    // claiming it, so only a present dial reaches the macOS Input Monitoring
    // prompt. HID encoders stay opt-in because the first call into the HID
    // encoder backend triggers that prompt (kIOHIDOptionsTypeSeizeDevice
    // in the IOKit-direct backend, hid_open() in HIDAPI). Defaulting them off
    // means the prompt only ever fires for users who actually own and want to
    // use the hardware.
    {
        auto* group = new QGroupBox("USB Control Surfaces");
        auto* gvbox = new QVBoxLayout(group);
        gvbox->setSpacing(6);

        auto* note = new QLabel(
            "A connected Ulanzi Dial is detected and used automatically; turn "
            "it off to leave the dial's media keys to the operating system. "
            "Enable HID encoders only if you connect one. On macOS, AetherSDR "
            "asks for Input Monitoring permission the first time it claims "
            "one of these devices.");
        note->setWordWrap(true);
        setTone(note, Tone::Hint);
        gvbox->addWidget(note);

        auto* ulanziEnable = new QCheckBox("Use a Ulanzi Dial when detected");
        ulanziEnable->setChecked(UlanziDialMappings::enabled());
        connect(ulanziEnable, &QCheckBox::toggled, this, [this](bool on) {
            UlanziDialMappings::setEnabled(on);
            emit serialSettingsChanged();
        });
        gvbox->addWidget(ulanziEnable);

#ifdef HAVE_HIDAPI
        auto* hidEnable = new QCheckBox(
            "Enable HID encoders / StreamDeck+ (RC-28, PowerMate, ShuttleXpress, …)");
        hidEnable->setChecked(
            settings.value("HidEncoderEnabled", "False").toString() == "True");
        connect(hidEnable, &QCheckBox::toggled, this, [this](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("HidEncoderEnabled", on ? "True" : "False");
            s.save();
            emit serialSettingsChanged();
        });
        gvbox->addWidget(hidEnable);
#endif

        vbox->addWidget(group);
    }

    // ── Port Configuration ───────────────────────────────────────────────
    {
        auto* group = new QGroupBox("Port Configuration");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(4);

        // Port selector + manual entry for non-standard TTYs (#897)
        grid->addWidget(new QLabel("Port:"), 0, 0);
        auto* portCombo = new QComboBox;
        portCombo->setMinimumWidth(200);
        QString savedPort = settings.value("SerialPortName", "").toString();
        auto* customEdit = new QLineEdit;
        customEdit->setPlaceholderText("/dev/ttyr0");
        bool isCustom = populateSerialPortCombo(portCombo, customEdit, savedPort);
        // Refresh sits beside the port it rescans. The row keeps its own
        // width (rsPacked: no trailing stretch), so the grid still spreads
        // its caption: value pairs.
        auto* portRow = new QHBoxLayout;
        portRow->setProperty("rsPacked", true);
        portRow->addWidget(portCombo);
        grid->addLayout(portRow, 0, 1, 1, 3);

        auto* refreshBtn = new QPushButton("Refresh");
        // Let native style (notably macOS) drive the button height; a fixed
        // 24 px clipped the macOS button bezel and label baseline.
        refreshBtn->setMinimumHeight(24);
        portRow->addWidget(refreshBtn);

        // Custom port row — hidden unless "Custom..." selected or saved port is custom
        auto* customLabel = new QLabel("Path:");
        customLabel->setVisible(isCustom);
        customEdit->setVisible(isCustom);
        grid->addWidget(customLabel, 1, 0);
        grid->addWidget(customEdit, 1, 1, 1, 3);

        connect(portCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [portCombo, customLabel, customEdit](int idx) {
            bool custom = (portCombo->itemData(idx).toString() == "__custom__");
            customLabel->setVisible(custom);
            customEdit->setVisible(custom);
        });

        // Re-enumerate through the shared helper, which reselects by port NAME.
        // The hand-rolled version this replaces removed every row but
        // "Custom..." and then re-inserted ahead of it — leaving the selection
        // parked on "Custom..." whatever it had been, so pressing Refresh with
        // a real port chosen silently swapped the operator onto the manual-path
        // entry and revealed the Path row.
        auto reseedPorts = [combo = QPointer<QComboBox>(portCombo),
                            label = QPointer<QLabel>(customLabel),
                            edit = QPointer<QLineEdit>(customEdit)]() {
            if (!combo || !edit)
                return;
            const bool custom = refreshSerialPortCombo(combo, edit);
            if (label) label->setVisible(custom);
            edit->setVisible(custom);
        };
        connect(refreshBtn, &QPushButton::clicked, this, reseedPorts);
        m_serialPortReseeds.append(reseedPorts);

        // Baud rate
        grid->addWidget(new QLabel("Baud:"), 2, 0);
        auto* baudCombo = new QComboBox;
        for (int b : {9600, 19200, 38400, 57600, 115200})
            baudCombo->addItem(QString::number(b), b);
        int savedBaud = settings.value("SerialBaudRate", "9600").toInt();
        baudCombo->setCurrentIndex(baudCombo->findData(savedBaud));
        grid->addWidget(baudCombo, 2, 1);

        // Data bits
        grid->addWidget(new QLabel("Data:"), 2, 2);
        auto* dataCombo = new QComboBox;
        dataCombo->addItem("8", 8);
        dataCombo->addItem("7", 7);
        int savedData = settings.value("SerialDataBits", "8").toInt();
        dataCombo->setCurrentIndex(dataCombo->findData(savedData));
        grid->addWidget(dataCombo, 2, 3);

        // Parity
        grid->addWidget(new QLabel("Parity:"), 3, 0);
        auto* parityCombo = new QComboBox;
        parityCombo->addItem("None", 0);
        parityCombo->addItem("Even", 2);
        parityCombo->addItem("Odd", 3);
        int savedParity = settings.value("SerialParity", "0").toInt();
        parityCombo->setCurrentIndex(parityCombo->findData(savedParity));
        grid->addWidget(parityCombo, 3, 1);

        // Stop bits
        grid->addWidget(new QLabel("Stop:"), 3, 2);
        auto* stopCombo = new QComboBox;
        stopCombo->addItem("1", 1);
        stopCombo->addItem("2", 2);
        int savedStop = settings.value("SerialStopBits", "1").toInt();
        stopCombo->setCurrentIndex(stopCombo->findData(savedStop));
        grid->addWidget(stopCombo, 3, 3);

        vbox->addWidget(group);

        // Save port settings on any change
        auto savePort = [portCombo, customEdit, baudCombo, dataCombo, parityCombo, stopCombo]() {
            auto& s = AppSettings::instance();
            QString port = portCombo->currentData().toString();
            if (port == "__custom__") {
                port = customEdit->text().trimmed();
            }
            s.setValue("SerialPortName", port);
            s.setValue("SerialBaudRate", baudCombo->currentData().toString());
            s.setValue("SerialDataBits", dataCombo->currentData().toString());
            s.setValue("SerialParity", parityCombo->currentData().toString());
            s.setValue("SerialStopBits", stopCombo->currentData().toString());
            s.save();
        };
        connect(portCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, savePort);
        connect(customEdit, &QLineEdit::textChanged, this, savePort);
        connect(baudCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, savePort);
        connect(dataCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, savePort);
        connect(parityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, savePort);
        connect(stopCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, savePort);
    }

    // ── Pin Assignment ───────────────────────────────────────────────────
    {
        auto* group = new QGroupBox("Pin Assignment");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(4);

        auto* headerPin = new QLabel("Pin");
        auto* headerFn  = new QLabel("Function");
        auto* headerPol = new QLabel("Polarity");
        // Column headings in the heading tone, as on the Antennas page.
        setTone(headerPin, Tone::Heading);
        setTone(headerFn, Tone::Heading);
        setTone(headerPol, Tone::Heading);
        grid->addWidget(headerPin, 0, 0);
        grid->addWidget(headerFn,  0, 1);
        grid->addWidget(headerPol, 0, 2);

        auto makeFnCombo = [this](const QString& savedKey) {
            auto* combo = new QComboBox;
            combo->addItem("None",   "None");
            combo->addItem("PTT",    "PTT");
            combo->addItem("CW Key", "CwKey");
            combo->addItem("CW PTT", "CwPTT");
            QString saved = AppSettings::instance().value(savedKey, "None").toString();
            for (int i = 0; i < combo->count(); ++i)
                if (combo->itemData(i).toString() == saved) { combo->setCurrentIndex(i); break; }
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [savedKey, combo]() {
                auto& s = AppSettings::instance();
                s.setValue(savedKey, combo->currentData().toString());
                s.save();
            });
            return combo;
        };

        auto makePolCombo = [this](const QString& savedKey) {
            auto* combo = new QComboBox;
            combo->addItem("Active High", "ActiveHigh");
            combo->addItem("Active Low",  "ActiveLow");
            QString saved = AppSettings::instance().value(savedKey, "ActiveHigh").toString();
            combo->setCurrentIndex(saved == "ActiveLow" ? 1 : 0);
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [savedKey, combo]() {
                auto& s = AppSettings::instance();
                s.setValue(savedKey, combo->currentData().toString());
                s.save();
            });
            return combo;
        };

        // DTR row
        grid->addWidget(new QLabel("DTR"), 1, 0);
        grid->addWidget(makeFnCombo("SerialDtrFunction"), 1, 1);
        grid->addWidget(makePolCombo("SerialDtrPolarity"), 1, 2);

        // RTS row
        grid->addWidget(new QLabel("RTS"), 2, 0);
        grid->addWidget(makeFnCombo("SerialRtsFunction"), 2, 1);
        grid->addWidget(makePolCombo("SerialRtsPolarity"), 2, 2);

        // Input pin function combo (different options than output)
        auto makeInputFnCombo = [this](const QString& savedKey) {
            auto* combo = new QComboBox;
            combo->addItem("None",         "None");
            combo->addItem("PTT Input",    "PttInput");
            combo->addItem("CW Key Input", "CwKeyInput");
            combo->addItem("CW Dit Input", "CwDitInput");
            combo->addItem("CW Dah Input", "CwDahInput");
            QString saved = AppSettings::instance().value(savedKey, "None").toString();
            for (int i = 0; i < combo->count(); ++i)
                if (combo->itemData(i).toString() == saved) { combo->setCurrentIndex(i); break; }
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [savedKey, combo]() {
                auto& s = AppSettings::instance();
                s.setValue(savedKey, combo->currentData().toString());
                s.save();
            });
            return combo;
        };

        // CTS row (input)
        auto* ctsLabel = new QLabel("CTS");
        applyLabelStyle(ctsLabel);
        grid->addWidget(ctsLabel, 3, 0);
        grid->addWidget(makeInputFnCombo("SerialCtsFunction"), 3, 1);
        grid->addWidget(makePolCombo("SerialCtsPolarity"), 3, 2);

        // DSR row (input)
        auto* dsrLabel = new QLabel("DSR");
        applyLabelStyle(dsrLabel);
        grid->addWidget(dsrLabel, 4, 0);
        grid->addWidget(makeInputFnCombo("SerialDsrFunction"), 4, 1);
        grid->addWidget(makePolCombo("SerialDsrPolarity"), 4, 2);

        // DCD row (input) — added for accessories that wire to the FTDI
        // chip's DCD# pin (e.g. HaliKey Serial from Halibut Electronics,
        // whose TRS Ring is wired to both DSR and DCD).
        auto* dcdLabel = new QLabel("DCD");
        applyLabelStyle(dcdLabel);
        grid->addWidget(dcdLabel, 5, 0);
        grid->addWidget(makeInputFnCombo("SerialDcdFunction"), 5, 1);
        grid->addWidget(makePolCombo("SerialDcdPolarity"), 5, 2);

        // Paddle swap
        auto* swapCb = new QCheckBox("Paddle Swap (swap dit/dah)");
        swapCb->setChecked(AppSettings::instance().value("SerialPaddleSwap", "False").toString() == "True");
        connect(swapCb, &QCheckBox::toggled, this, [](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("SerialPaddleSwap", on ? "True" : "False");
            s.save();
        });
        grid->addWidget(swapCb, 6, 0, 1, 3);

        vbox->addWidget(group);
    }

    // ── Open / Close / Auto-open ────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(8);


        auto* openBtn = new QPushButton("Open");
        // Min-width rather than fixed-width: macOS native button metrics need
        // more horizontal room than the 80 px Windows/Fusion baseline.
        openBtn->setMinimumWidth(80);
        setButtonRole(openBtn, ButtonRole::Select);
        auto* closeBtn = new QPushButton("Close");
        closeBtn->setMinimumWidth(80);
        setButtonRole(closeBtn, ButtonRole::Select);

        auto* statusLabel = new QLabel;

        row->addWidget(openBtn);
        row->addWidget(closeBtn);
        row->addWidget(statusLabel);
        row->addStretch();

        auto updatePortStatus = [openBtn, closeBtn, statusLabel]() {
            bool open = AppSettings::instance().value("SerialPortOpen", "False").toString() == "True";
            openBtn->setEnabled(!open);
            closeBtn->setEnabled(open);
            if (open) {
                statusLabel->setText("Open");
                setTone(statusLabel, Tone::Ok);
            } else {
                statusLabel->setText("Closed");
                setTone(statusLabel, Tone::Hint);
            }
        };

        connect(openBtn, &QPushButton::clicked, this, [this, updatePortStatus]() {
            auto& s = AppSettings::instance();
            s.setValue("SerialPortOpen", "True");
            s.save();
            updatePortStatus();
            emit serialSettingsChanged();
        });

        connect(closeBtn, &QPushButton::clicked, this, [this, updatePortStatus]() {
            auto& s = AppSettings::instance();
            s.setValue("SerialPortOpen", "False");
            s.save();
            updatePortStatus();
            emit serialSettingsChanged();
        });

        updatePortStatus();
        // In a group like everything else on the page, not loose between
        // Pin Assignment and the FlexControl knob.
        auto* connectionGroup = new QGroupBox("Connection");
        auto* connectionBox = new QVBoxLayout(connectionGroup);
        connectionBox->setSpacing(6);
        connectionBox->addLayout(row);
        vbox->addWidget(connectionGroup);

        auto* autoOpen = new QCheckBox("Auto-open serial port on startup");
        autoOpen->setChecked(settings.value("SerialAutoOpen", "False").toString() == "True");
        connect(autoOpen, &QCheckBox::toggled, this, [](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("SerialAutoOpen", on ? "True" : "False");
            s.save();
        });
        connectionBox->addWidget(autoOpen);
    }

    // ── FlexControl tuning knob ────────────────────────────────────────
    {
        auto* group = new QGroupBox("FlexControl Tuning Knob");
        m_flexControlGroup = group;
        // Host peripheral, not a radio capability -- see updateRadioCapabilityVisibility (#5778).
        group->setVisible(true);
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        // Status
        auto* fcStatusLabel = new QLabel("Not detected");
        setTone(fcStatusLabel, Tone::Hint);
        m_flexControlStatusLabel = fcStatusLabel;
        grid->addWidget(new QLabel("Status:"), 0, 0);
        grid->addWidget(fcStatusLabel, 0, 1);

        // Detect / Close buttons
        auto* fcDetectBtn = new QPushButton("Detect");
        // Same macOS-native-metrics consideration as the Open/Close pair above.
        fcDetectBtn->setMinimumWidth(80);
        auto* fcCloseBtn = new QPushButton("Close");
        fcCloseBtn->setMinimumWidth(80);
        fcCloseBtn->setEnabled(false);
        m_flexControlDetectButton = fcDetectBtn;
        m_flexControlCloseButton = fcCloseBtn;

        auto* btnRow = new QHBoxLayout;
        btnRow->addWidget(fcDetectBtn);
        btnRow->addWidget(fcCloseBtn);
        btnRow->addStretch();
        grid->addLayout(btnRow, 0, 2);

        // Update status display
        connect(fcDetectBtn, &QPushButton::clicked, this, [this] {
            QString port = FlexControlManager::detectPort();
            if (port.isEmpty()) {
                setFlexControlConnectionStatus(false);
                return;
            }
            // Store port for MainWindow to open
            auto& s = AppSettings::instance();
            s.setValue("FlexControlPort", port);
            s.setValue("FlexControlOpen", "True");
            s.save();
            emit serialSettingsChanged();
        });
        connect(fcCloseBtn, &QPushButton::clicked, this, [this] {
            auto& s = AppSettings::instance();
            s.setValue("FlexControlOpen", "False");
            s.save();
            setFlexControlConnectionStatus(false);
            emit serialSettingsChanged();
        });

        // Show current state from settings
        if (settings.value("FlexControlOpen", "False").toString() == "True") {
            QString port = settings.value("FlexControlPort").toString();
            if (!port.isEmpty())
                setFlexControlConnectionStatus(true, port);
        }

        // Button action configuration
        static const QStringList actions = {
            "None", "StepUp", "StepDown", "ToggleMox",
            "ToggleTune", "ToggleMute", "ToggleLock",
            "BandZoom", "SegmentZoom",
            "NextSlice", "PrevSlice",
            "SplitActiveSlice",
            "SplitMonitorTx",
            "ToggleAgc", "VolumeUp", "VolumeDown",
            "WheelFrequency", "WheelVolume", "WheelPower",
            "WheelRit", "WheelXit",
            "WheelSliceAudio",
            "WheelHeadphoneVolume",
            "WheelAgcT", "WheelApf", "WheelCwSpeed",
            "ClearRit", "ClearXit", "ToggleApf",
            "CwxF1", "CwxF2", "CwxF3", "CwxF4",
            "CwxF5", "CwxF6", "CwxF7", "CwxF8",
            "CwxF9", "CwxF10", "CwxF11", "CwxF12"
        };
        static const char* defaultActions[4][2] = {
            {"StepUp", "StepDown"},
            {"ToggleMox", "ToggleTune"},
            {"ToggleMute", "ToggleLock"},
            {"StepUp", "StepDown"},      // Knob button
        };
        static const char* btnLabels[4] = {"Button 1:", "Button 2:", "Button 3:", "Knob Button:"};
        static const char* actLabels[2] = {"Tap", "Double"};

        for (int b = 0; b < 4; ++b) {
            grid->addWidget(new QLabel(btnLabels[b]), b + 1, 0);
            auto* row = new QHBoxLayout;
            for (int a = 0; a < 2; ++a) {
                row->addWidget(new QLabel(actLabels[a]));
                auto* combo = new QComboBox;
                combo->addItems(actions);
                QString key = QString("FlexControlBtn%1Action%2").arg(b + 1).arg(a);
                QString current = settings.value(key, defaultActions[b][a]).toString();
                int idx = actions.indexOf(current);
                if (idx >= 0) combo->setCurrentIndex(idx);
                m_flexControlActionCombos.insert(key, combo);
                m_flexControlActionDefaults.insert(key, QString::fromLatin1(defaultActions[b][a]));
                connect(combo, &QComboBox::currentTextChanged, this, [this, key](const QString& text) {
                    auto& s = AppSettings::instance();
                    s.setValue(key, text);
                    s.save();
                    emit serialSettingsChanged();
                });
                row->addWidget(combo);
            }
            row->addStretch();
            grid->addLayout(row, b + 1, 1, 1, 2);
        }

        // Auto-detect checkbox
        auto* autoDetect = new QCheckBox("Auto-detect on startup");
        autoDetect->setChecked(settings.value("FlexControlAutoDetect", "True").toString() == "True");
        connect(autoDetect, &QCheckBox::toggled, this, [this](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("FlexControlAutoDetect", on ? "True" : "False");
            s.save();
            emit serialSettingsChanged();
        });
        grid->addWidget(autoDetect, 5, 0, 1, 3);

        auto* invertDir = new QCheckBox("Invert tuning direction");
        invertDir->setChecked(settings.value("FlexControlInvertDir", "False").toString() == "True");
        m_flexControlInvertCheck = invertDir;
        connect(invertDir, &QCheckBox::toggled, this, [this](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("FlexControlInvertDir", on ? "True" : "False");
            s.save();
            emit serialSettingsChanged();
        });
        grid->addWidget(invertDir, 6, 0, 1, 3);

        vbox->addWidget(group);
    }

    // ── HID / StreamDeck+ LCD button action mapping (#1510) ──────────────────
#ifdef HAVE_HIDAPI
    {
        auto* group = new QGroupBox("StreamDeck+ LCD Button Actions");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        auto* note = new QLabel(
            "Assign an action to each of the 8 LCD buttons. "
            "The button label updates on the device to match.");
        note->setWordWrap(true);
        setTone(note, Tone::Hint);
        grid->addWidget(note, 0, 0, 1, 4);

        static const struct { const char* id; const char* label; } kKeyActions[] = {
            {"None",             "None"},
            {"ToggleMox",        "Toggle TX (MOX)"},
            {"ToggleTune",       "Toggle Tune"},
            {"ToggleRit",        "Toggle RIT on/off"},
            {"ToggleXit",        "Toggle XIT on/off"},
            {"ClearRit",         "Clear RIT offset"},
            {"ClearXit",         "Clear XIT offset"},
            {"StepUp",           "Step Size Up"},
            {"StepDown",         "Step Size Down"},
            {"ToggleMute",       "Toggle Mute"},
            {"ToggleLock",       "Toggle Slice Lock"},
            {"ToggleApf",        "Toggle APF"},
            {"ToggleAgc",        "Cycle AGC Mode"},
            {"BandZoom",         "Toggle Band Zoom"},
            {"SegmentZoom",      "Toggle Segment Zoom"},
            {"NextSlice",        "Next Slice"},
            {"PrevSlice",        "Previous Slice"},
            {"VolumeUp",         "Volume Up (+5)"},
            {"VolumeDown",       "Volume Down (-5)"},
            {"SplitActiveSlice", "Toggle Split"},
            {"SplitMonitorTx",   "Monitor TX Frequency"},
        };

        // 8 keys laid out as 2 columns of 4
        for (int i = 0; i < 8; ++i) {
            const int row = (i % 4) + 1;
            const int col = (i / 4) * 2;

            grid->addWidget(new QLabel(QString("Key %1:").arg(i + 1)), row, col);

            auto* combo = new QComboBox;
            for (const auto& act : kKeyActions)
                combo->addItem(QString::fromLatin1(act.label), QString::fromLatin1(act.id));

            const QString key    = QString("HidKeyAction%1").arg(i);
            const QString saved  = settings.value(key, QStringLiteral("None")).toString();
            const int     selIdx = combo->findData(saved);
            combo->setCurrentIndex(selIdx >= 0 ? selIdx : 0);

            connect(combo, &QComboBox::currentIndexChanged, this, [combo, key, this](int) {
                auto& s = AppSettings::instance();
                s.setValue(key, combo->currentData().toString());
                s.save();
                emit serialSettingsChanged();
            });

            m_hidKeyActionCombos[i] = combo;
            grid->addWidget(combo, row, col + 1);
        }
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);

        vbox->addWidget(group);
    }

    // ── HID Encoder — per-encoder action mapping (#1510) ─────────────────────
    {
        auto* group = new QGroupBox("HID Encoder / StreamDeck+ Encoders");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        auto* note = new QLabel(
            "Assign an action to each encoder dial. "
            "Single-encoder devices (RC-28, PowerMate, ShuttleXpress) use Encoder 1 only.");
        note->setWordWrap(true);
        setTone(note, Tone::Hint);
        grid->addWidget(note, 0, 0, 1, 3);

        static const struct { const char* id; const char* label; } kEncoderActions[] = {
            {"WheelFrequency",      "Tune Slice"},
            {"WheelRit",            "RIT (Receive Incremental Tuning)"},
            {"WheelXit",            "XIT (Transmit Incremental Tuning)"},
            {"WheelVolume",         "Master Volume"},
            {"WheelSliceAudio",     "Slice Audio Volume"},
            {"WheelHeadphoneVolume","Headphone Volume"},
            {"WheelAgcT",           "AGC Threshold"},
            {"WheelApf",            "APF Level"},
            {"WheelCwSpeed",        "CW Speed"},
            {"WheelPower",          "RF Power"},
            {"None",                "None"},
        };

        static const char* kEncoderDefaults[4] = {
            "WheelFrequency", "WheelRit", "WheelXit", "WheelVolume"
        };

        for (int i = 0; i < 4; ++i) {
            grid->addWidget(new QLabel(QString("Encoder %1:").arg(i + 1)), i + 1, 0);

            auto* combo = new QComboBox;
            for (const auto& act : kEncoderActions)
                combo->addItem(QString::fromLatin1(act.label), QString::fromLatin1(act.id));

            const QString key = QString("HidEncoderAction%1").arg(i);
            const QString saved = settings.value(key, QString::fromLatin1(kEncoderDefaults[i])).toString();
            const int idx = combo->findData(saved);
            combo->setCurrentIndex(idx >= 0 ? idx : 0);

            connect(combo, &QComboBox::currentIndexChanged, this, [combo, key, this](int) {
                auto& s = AppSettings::instance();
                s.setValue(key, combo->currentData().toString());
                s.save();
                emit serialSettingsChanged();
            });

            m_hidEncoderActionCombos[i] = combo;
            grid->addWidget(combo, i + 1, 1, 1, 2);
            grid->setColumnStretch(1, 1);
        }

        vbox->addWidget(group);
    }

    // ── HID Encoder — per-encoder push-button action mapping (#1510) ─────────
    {
        auto* group = new QGroupBox("StreamDeck+ Encoder Push Actions");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        auto* note = new QLabel(
            "Action when each encoder dial is pressed. "
            "Defaults: Enc 1 = Cycle Tuning Step, Enc 2 = Toggle RIT, Enc 3 = Toggle XIT.");
        note->setWordWrap(true);
        setTone(note, Tone::Hint);
        grid->addWidget(note, 0, 0, 1, 3);

        static const struct { const char* id; const char* label; } kPushActions[] = {
            {"StepCycle",  "Cycle Tuning Step"},
            {"ToggleRit",  "Toggle RIT on/off"},
            {"ToggleXit",  "Toggle XIT on/off"},
            {"ToggleMox",  "Toggle TX (MOX)"},
            {"ToggleMute", "Toggle Mute"},
            {"ToggleLock", "Lock Slice"},
            {"None",       "None"},
        };

        static const char* kPushDefaults[4] = {
            "StepCycle", "ToggleRit", "ToggleXit", "None"
        };

        for (int i = 0; i < 4; ++i) {
            grid->addWidget(new QLabel(QString("Encoder %1 push:").arg(i + 1)), i + 1, 0);

            auto* combo = new QComboBox;
            for (const auto& act : kPushActions)
                combo->addItem(QString::fromLatin1(act.label), QString::fromLatin1(act.id));

            const QString key = QString("HidEncoderPushAction%1").arg(i);
            const QString saved = settings.value(key, QString::fromLatin1(kPushDefaults[i])).toString();
            const int idx = combo->findData(saved);
            combo->setCurrentIndex(idx >= 0 ? idx : 0);

            connect(combo, &QComboBox::currentIndexChanged, this, [combo, key, this](int) {
                auto& s = AppSettings::instance();
                s.setValue(key, combo->currentData().toString());
                s.save();
                emit serialSettingsChanged();
            });

            m_hidEncoderPushActionCombos[i] = combo;
            grid->addWidget(combo, i + 1, 1, 1, 2);
            grid->setColumnStretch(1, 1);
        }

        vbox->addWidget(group);
    }

    // --- TMate 2 device actions, encoders, and backlight ---------------------
    {
        auto* group = new QGroupBox("TMate 2 Key Actions");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        auto* note = new QLabel("Assign actions to the six TMate 2 function keys.");
        note->setWordWrap(true);
        setTone(note, Tone::Hint);
        grid->addWidget(note, 0, 0, 1, 4);

        static const struct { const char* id; const char* label; } kTMate2KeyActions[] = {
            {"None",             "None"},
            {"ToggleMox",        "Toggle TX (MOX)"},
            {"ToggleTune",       "Toggle Tune"},
            {"ToggleRit",        "Toggle RIT on/off"},
            {"ToggleXit",        "Toggle XIT on/off"},
            {"ClearRit",         "Clear RIT offset"},
            {"ClearXit",         "Clear XIT offset"},
            {"StepUp",           "Step Size Up"},
            {"StepDown",         "Step Size Down"},
            {"ToggleMute",       "Toggle Mute"},
            {"ToggleLock",       "Toggle Slice Lock"},
            {"ToggleApf",        "Toggle APF"},
            {"ToggleAgc",        "Cycle AGC Mode"},
            {"BandZoom",         "Toggle Band Zoom"},
            {"SegmentZoom",      "Toggle Segment Zoom"},
            {"NextSlice",        "Next Slice"},
            {"PrevSlice",        "Previous Slice"},
            {"VolumeUp",         "Volume Up (+5)"},
            {"VolumeDown",       "Volume Down (-5)"},
            {"SplitActiveSlice", "Toggle Split"},
            {"SplitMonitorTx",   "Monitor TX Frequency"},
        };

        static const char* kTMate2KeyDefaults[6] = {
            "ToggleMox", "ToggleAgc", "BandZoom", "ToggleApf", "ToggleMute", "ToggleRit"
        };

        for (int i = 0; i < 6; ++i) {
            const int row = (i % 3) + 1;
            const int col = (i / 3) * 2;
            grid->addWidget(new QLabel(QString("F%1:").arg(i + 1)), row, col);

            auto* combo = new QComboBox;
            for (const auto& act : kTMate2KeyActions)
                combo->addItem(QString::fromLatin1(act.label), QString::fromLatin1(act.id));

            const QString key = QString("TMate2KeyAction%1").arg(i);
            const QString saved = settings.value(
                key, QString::fromLatin1(kTMate2KeyDefaults[i])).toString();
            const int idx = combo->findData(saved);
            combo->setCurrentIndex(idx >= 0 ? idx : 0);

            connect(combo, &QComboBox::currentIndexChanged, this, [combo, key, this](int) {
                auto& s = AppSettings::instance();
                s.setValue(key, combo->currentData().toString());
                s.save();
                emit serialSettingsChanged();
            });

            m_tmate2KeyActionCombos[i] = combo;
            grid->addWidget(combo, row, col + 1);
        }
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);

        vbox->addWidget(group);
    }

    {
        auto* group = new QGroupBox("TMate 2 Encoder Actions");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        auto* note = new QLabel("Assign actions to the three TMate 2 encoder dials.");
        note->setWordWrap(true);
        setTone(note, Tone::Hint);
        grid->addWidget(note, 0, 0, 1, 3);

        static const struct { const char* id; const char* label; } kTMate2EncoderActions[] = {
            {"WheelFrequency",      "Tune Slice"},
            {"WheelRit",            "RIT (Receive Incremental Tuning)"},
            {"WheelXit",            "XIT (Transmit Incremental Tuning)"},
            {"WheelVolume",         "Master Volume"},
            {"WheelSliceAudio",     "Slice Audio Volume"},
            {"WheelHeadphoneVolume","Headphone Volume"},
            {"WheelAgcT",           "AGC Threshold"},
            {"WheelApf",            "APF Level"},
            {"WheelCwSpeed",        "CW Speed"},
            {"WheelPower",          "RF Power"},
            {"None",                "None"},
        };

        static const char* kTMate2EncoderDefaults[3] = {
            "WheelFrequency", "WheelRit", "WheelXit"
        };

        for (int i = 0; i < 3; ++i) {
            grid->addWidget(new QLabel(QString("Encoder %1:").arg(i + 1)), i + 1, 0);

            auto* combo = new QComboBox;
            for (const auto& act : kTMate2EncoderActions)
                combo->addItem(QString::fromLatin1(act.label), QString::fromLatin1(act.id));

            const QString key = QString("TMate2EncoderAction%1").arg(i);
            const QString saved = settings.value(
                key, QString::fromLatin1(kTMate2EncoderDefaults[i])).toString();
            const int idx = combo->findData(saved);
            combo->setCurrentIndex(idx >= 0 ? idx : 0);

            connect(combo, &QComboBox::currentIndexChanged, this, [combo, key, this](int) {
                auto& s = AppSettings::instance();
                s.setValue(key, combo->currentData().toString());
                s.save();
                emit serialSettingsChanged();
            });

            m_tmate2EncoderActionCombos[i] = combo;
            grid->addWidget(combo, i + 1, 1, 1, 2);
            grid->setColumnStretch(1, 1);
        }

        vbox->addWidget(group);
    }

    {
        auto* group = new QGroupBox("TMate 2 Encoder Push Actions");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        auto* note = new QLabel("Action when each TMate 2 encoder is pressed.");
        note->setWordWrap(true);
        setTone(note, Tone::Hint);
        grid->addWidget(note, 0, 0, 1, 3);

        static const struct { const char* id; const char* label; } kTMate2PushActions[] = {
            {"StepCycle",  "Cycle Tuning Step"},
            {"ToggleRit",  "Toggle RIT on/off"},
            {"ToggleXit",  "Toggle XIT on/off"},
            {"ToggleMox",  "Toggle TX (MOX)"},
            {"ToggleMute", "Toggle Mute"},
            {"ToggleLock", "Lock Slice"},
            {"None",       "None"},
        };

        static const char* kTMate2PushDefaults[3] = {
            "StepCycle", "ToggleRit", "ToggleXit"
        };

        for (int i = 0; i < 3; ++i) {
            grid->addWidget(new QLabel(QString("Encoder %1 push:").arg(i + 1)), i + 1, 0);

            auto* combo = new QComboBox;
            for (const auto& act : kTMate2PushActions)
                combo->addItem(QString::fromLatin1(act.label), QString::fromLatin1(act.id));

            const QString key = QString("TMate2PushAction%1").arg(i);
            const QString saved = settings.value(
                key, QString::fromLatin1(kTMate2PushDefaults[i])).toString();
            const int idx = combo->findData(saved);
            combo->setCurrentIndex(idx >= 0 ? idx : 0);

            connect(combo, &QComboBox::currentIndexChanged, this, [combo, key, this](int) {
                auto& s = AppSettings::instance();
                s.setValue(key, combo->currentData().toString());
                s.save();
                emit serialSettingsChanged();
            });

            m_tmate2EncoderPushActionCombos[i] = combo;
            grid->addWidget(combo, i + 1, 1, 1, 2);
            grid->setColumnStretch(1, 1);
        }

        vbox->addWidget(group);
    }

    {
        auto* group = new QGroupBox("TMate 2 Display");
        auto* grid = new QGridLayout(group);
        grid->setSpacing(6);

        auto* note = new QLabel(
            "Backlight colors and temporary display feedback. "
            "Overlay duration controls how long changed values are shown on the TMate 2 LCD.");
        note->setWordWrap(true);
        setTone(note, Tone::Hint);
        grid->addWidget(note, 0, 0, 1, 7);

        static const struct {
            const char* rowLabel;
            const char* rKey; const char* gKey; const char* bKey;
            const char* rDflt; const char* gDflt; const char* bDflt;
            int spinOffset;
        } kRows[2] = {
            {"RX backlight:", "TMate2BacklightR",   "TMate2BacklightG",   "TMate2BacklightB",
                              "0",                  "50",                 "255", 0},
            {"TX backlight:", "TMate2TxBacklightR", "TMate2TxBacklightG", "TMate2TxBacklightB",
                              "255",                "30",                 "0",   3},
        };

        static const char* kLabels[3] = {"R:", "G:", "B:"};
        for (int row = 0; row < 2; ++row) {
            auto* rowLbl = new QLabel(QString::fromLatin1(kRows[row].rowLabel));
            applyLabelStyle(rowLbl);
            grid->addWidget(rowLbl, row + 1, 0);
            const char* keys[3] = {kRows[row].rKey, kRows[row].gKey, kRows[row].bKey};
            const char* dflts[3] = {kRows[row].rDflt, kRows[row].gDflt, kRows[row].bDflt};
            for (int ch = 0; ch < 3; ++ch) {
                auto* lbl = new QLabel(QString::fromLatin1(kLabels[ch]));
                applyLabelStyle(lbl);
                grid->addWidget(lbl, row + 1, 1 + ch * 2);

                auto* spin = new QSpinBox;
                spin->setRange(0, 255);
                spin->setValue(settings.value(keys[ch], dflts[ch]).toInt());
                // 0–255: three digits and the steppers.
                spin->setFixedWidth(76);
                grid->addWidget(spin, row + 1, 2 + ch * 2);
                m_tmate2BacklightSpins[kRows[row].spinOffset + ch] = spin;

                const QString key = QString::fromLatin1(keys[ch]);
                connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                        this, [this, key](int val) {
                    auto& s = AppSettings::instance();
                    s.setValue(key, QString::number(val));
                    s.save();
                    emit serialSettingsChanged();
                });
            }
        }

        auto addTimingSpin = [&](int row, const QString& label, const QString& key,
                                 int dflt, int min, int max, int step) -> QSpinBox* {
            auto* lbl = new QLabel(label);
            applyLabelStyle(lbl);
            grid->addWidget(lbl, row, 0, 1, 2);

            auto* spin = new QSpinBox;
            spin->setRange(min, max);
            spin->setSingleStep(step);
            spin->setSuffix(" ms");
            spin->setValue(settings.value(key, QString::number(dflt)).toInt());
            spin->setFixedWidth(120);
            grid->addWidget(spin, row, 2, 1, 2, Qt::AlignLeft);

            connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                    this, [this, key](int val) {
                auto& s = AppSettings::instance();
                s.setValue(key, QString::number(val));
                s.save();
                emit serialSettingsChanged();
            });
            return spin;
        };

        m_tmate2OverlayDurationSpin = addTimingSpin(
            3, "Overlay duration:", "TMate2OverlayDurationMs", 1500, 100, 10000, 100);
        m_tmate2UserInteractionTimeoutSpin = addTimingSpin(
            4, "User interaction timeout:", "TMate2UserInteractionTimeoutMs", 2000, 0, 60000, 100);

        // The fields keep their widths; the group's spare width is at its end.
        grid->setColumnStretch(7, 1);

        vbox->addWidget(group);
    }

#endif

    vbox->addStretch();
    return page;
}
#endif



// ─── Peripherals tab — manual IP connect for TGXL, PGXL, AG (#914) ───────────

namespace {
std::function<bool(const QString&, const QString&)>& removalConfirmationHook()
{
    static std::function<bool(const QString&, const QString&)> hook;
    return hook;
}
} // namespace

void RadioSetupDialog::setRemovalConfirmationHookForTest(
    std::function<bool(const QString&, const QString&)> hook)
{
    removalConfirmationHook() = std::move(hook);
}

bool RadioSetupDialog::confirmPeripheralRemoval(const QString& label, const QString& toggleLabel)
{
    // Only the network devices with an authorization code have a toggle.
    const QString text = toggleLabel.isEmpty()
        ? tr("Remove %1?\n\n"
             "This disconnects it and clears its saved connection settings.").arg(label)
        : tr("Remove %1?\n\n"
             "This disconnects it and clears its saved connection settings and any stored "
             "authorization code. \"%2\" returns to its default (on).\n\n"
             "Remove does not stop discovery: a device the radio or your network reports may "
             "connect again. To keep a device from connecting by itself, leave it in the list "
             "and turn off %2 instead.").arg(label, toggleLabel);
    if (const auto& hook = removalConfirmationHook()) {
        return hook(label, text);
    }
    const QPointer<RadioSetupDialog> self(this);
    ScopedChildWidget<QMessageBox> boxOwner(
        QMessageBox::Warning, tr("Remove %1").arg(label), text,
        QMessageBox::Ok | QMessageBox::Cancel, this);
    boxOwner.get()->setObjectName(QStringLiteral("peripheralRemoveConfirmation"));
    boxOwner.get()->setDefaultButton(QMessageBox::Cancel);
    boxOwner.get()->button(QMessageBox::Ok)->setText(tr("Remove"));
    const int ret = boxOwner.get()->exec();
    return self && boxOwner && ret == QMessageBox::Ok;
}

void RadioSetupDialog::buildDeferredTab(int index)
{
    auto it = m_deferredBuilders.find(index);
    if (it == m_deferredBuilders.end())
        return;                             // already built or out of range

    QWidget* placeholder = m_pages->widget(index);
    QWidget* content = it.value()();        // run the real builder
    auto* lay = new QVBoxLayout(placeholder);
    lay->setContentsMargins(0, 0, 0, 0);
    // Wrap in a scroll area so tall tabs (Themes, Audio, Filters,
    // Peripherals on small / high-DPI displays) become scrollable instead
    // of forcing the dialog past the screen edge (#3345).
    lay->addWidget(wrapTabInScrollArea(content));
    m_deferredBuilders.erase(it);           // build only once
}

void RadioSetupDialog::selectTab(const QString& tabName)
{
    if (!m_navigation) {
        return;
    }

    static const QHash<QString, QString> kLegacyPageNames = {
        {QStringLiteral("TX"), QStringLiteral("Transmit")},
        {QStringLiteral("RX"), QStringLiteral("Receive")},
        {QStringLiteral("Phone/CW"), QStringLiteral("Phone & CW")},
        {QStringLiteral("XVTR"), QStringLiteral("Transverters")},
        {QStringLiteral("Themes"), QStringLiteral("Appearance & Behavior")},
        {QStringLiteral("QRZ"), QStringLiteral("QRZ & Callsigns")},
        {QStringLiteral("Serial"), QStringLiteral("Serial & Controllers")}
    };
    const QString pageName = kLegacyPageNames.value(tabName, tabName);
    const int index = m_pageIndexes.value(pageName, -1);
    if (QTreeWidgetItem* item = m_pageItems.value(index, nullptr)) {
        if (!isCapabilityPageAvailable(item)) {
            return;
        }
        if (isGpsPage(item) && !isGpsSetupAvailable()) {
            return;
        }
        m_navigation->setCurrentItem(item);
        m_navigation->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
}

void RadioSetupDialog::revealFlexControlSettings()
{
    // No capability check: the knob is a host serial device (#5778).
    selectTab(QStringLiteral("Serial & Controllers"));
    if (!m_flexControlGroup) {
        return;
    }
    // selectTab() just switched (and, on first visit, built) the page on
    // this call stack, but the scroll area it's wrapped in (#3345) hasn't
    // laid out yet — ensureWidgetVisible() against stale/zero geometry is a
    // no-op. Defer one event-loop turn so layout has actually happened.
    QPointer<QGroupBox> group = m_flexControlGroup;
    QTimer::singleShot(0, this, [group] {
        if (!group) {
            return;
        }
        // The group lives inside the tab's content widget, which
        // wrapTabInScrollArea() set as the QScrollArea's viewport child —
        // walk up the parent chain to find that enclosing scroll area.
        for (QWidget* w = group->parentWidget(); w; w = w->parentWidget()) {
            if (auto* area = qobject_cast<QScrollArea*>(w)) {
                QWidget* content = area->widget();
                if (!content) {
                    return;
                }
                // Deliberately not ensureWidgetVisible(): it *centers* a
                // widget taller than the viewport, and the FlexControl
                // Tuning Knob group (~450-480px) is tall enough that at the
                // dialog's 960x680 floor, centering pushes the group's own
                // title and Status row above the top edge — the opposite of
                // what "reveal" should do. Scroll its top edge into view
                // directly instead (PR #5157 review).
                const int y = group->mapTo(content, QPoint(0, 0)).y();
                area->verticalScrollBar()->setValue(qMax(0, y - 8));
                return;
            }
        }
    });
}

void RadioSetupDialog::refreshFlexControlButtonActions()
{
    auto& settings = AppSettings::instance();
    for (auto it = m_flexControlActionCombos.begin();
         it != m_flexControlActionCombos.end(); ++it) {
        auto* combo = it.value();
        if (!combo)
            continue;
        const QString fallback = m_flexControlActionDefaults.value(it.key(), QStringLiteral("None"));
        const QString saved = settings.value(it.key(), fallback).toString();
        const int idx = combo->findText(saved);
        if (idx < 0)
            continue;
        const QSignalBlocker blocker(combo);
        combo->setCurrentIndex(idx);
    }
    if (m_flexControlInvertCheck) {
        const bool inverted =
            settings.value("FlexControlInvertDir", "False").toString() == "True";
        const QSignalBlocker blocker(m_flexControlInvertCheck);
        m_flexControlInvertCheck->setChecked(inverted);
    }
}

void RadioSetupDialog::setFlexControlConnectionStatus(bool connected, const QString& port)
{
    if (m_flexControlStatusLabel) {
        if (connected) {
            const QString displayPort = port.isEmpty()
                ? AppSettings::instance().value("FlexControlPort").toString()
                : port;
            m_flexControlStatusLabel->setText(QString("Connected (%1)").arg(displayPort));
            setTone(m_flexControlStatusLabel, Tone::Ok);
        } else {
            m_flexControlStatusLabel->setText("Not detected");
            setTone(m_flexControlStatusLabel, Tone::Hint);
        }
    }
    if (m_flexControlCloseButton)
        m_flexControlCloseButton->setEnabled(connected);
    if (m_flexControlDetectButton)
        m_flexControlDetectButton->setEnabled(!connected);
}

void RadioSetupDialog::reportAutomationBridgeStartResult(bool ok)
{
    if (!m_automationBridgeBtn) {
        return;  // the Network tab has not been built
    }
    if (AutomationBridgeSettings::envForced()) {
        // The toggle is disabled and captioned "forced on" in this case, and
        // the saved opt-in was not touched; repainting it Disabled would
        // contradict both. The failure is in the log and the status bar.
        return;
    }
    const bool wasEnabled = m_automationBridgeBtn->isChecked();
    {
        // MainWindow owns persistence and rejects stale callbacks. Reconcile
        // even a dialog reopened during the pending read, whose initial toggle
        // still reflects the old setting. Do not emit a second start/stop.
        const QSignalBlocker blocker(m_automationBridgeBtn);
        m_automationBridgeBtn->setChecked(ok);
        m_automationBridgeBtn->setText(ok ? "Enabled" : "Disabled");
    }
    if (ok || !wasEnabled || !isVisible()) {
        return;
    }
    // Do not blame a sibling instance: AutomationServer::start() unlinks a
    // stale socket before listening, so on Unix a shared name is taken over,
    // not refused. What reaches here is a path-length or permission failure,
    // and the server already logged errorString().
    QMessageBox::warning(this, QStringLiteral("Agent Automation (MCP)"),
        QStringLiteral(
            "The automation bridge could not bind its socket — see the "
            "application log for the reason.\n\n"
            "MCP clients will not be able to connect. The toggle has been "
            "turned back off."));
}

// ── UI Enhancements tab ───────────────────────────────────────────────────────

QWidget* RadioSetupDialog::buildUiEnhancementsTab()
{
    // A swatch: the slice's own color, in a canon hairline.
    static const QString kBtnBase =
        "QPushButton { border: 1px solid {{color.canon.lineHi}}; border-radius: 3px; "
        "font-weight: bold; font-size: 13px; min-width: 36px; min-height: 36px; }"
        "QPushButton:hover { border: 2px solid {{color.canon.cyan}}; }";

    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(12);
    vbox->setContentsMargins(16, 16, 16, 16);

    // Slice letter display (#2606). "Global" (default): letters follow the
    // radio's global slot ('A' = slot 0) so Multi-Flex operators see which
    // slots are used. "RadioIndexed": the radio's per-client letter (as
    // SmartSDR) with the global slot as a subscript. Display only; IDs in
    // commands, settings keys and routing stay global.
    {
        auto* letterGrp = new QGroupBox("Slice Letter Display");
        auto* letterLayout = new QVBoxLayout(letterGrp);
        letterLayout->setSpacing(8);

        auto* radioRow = new QHBoxLayout;
        auto* globalRadio = new QRadioButton("Global slot index (A=0, B=1, …)");
        auto* radioIdxRadio =
            new QRadioButton("Radio-assigned letter with global subscript (A₂)");
        radioRow->addWidget(globalRadio);
        radioRow->addWidget(radioIdxRadio);
        radioRow->addStretch();
        letterLayout->addLayout(radioRow);

        auto* letterDesc = new QLabel(
            "Choose how slice letters are rendered in badges, faders, and "
            "applet labels. Multi-Flex sessions with multiple clients only: "
            "Radio-assigned letters match SmartSDR's behavior; the global "
            "subscript preserves which physical slot you're on.");
        setTone(letterDesc, Tone::Hint);
        letterDesc->setWordWrap(true);
        letterLayout->addWidget(letterDesc);

        auto& s = AppSettings::instance();
        const QString current =
            s.value("SliceLetterDisplay", "Global").toString();
        (current == "RadioIndexed" ? radioIdxRadio : globalRadio)->setChecked(true);

        auto saveLetterMode = [this](const QString& mode) {
            auto& s = AppSettings::instance();
            s.setValue("SliceLetterDisplay", mode);
            s.save();
            // Push a refresh through anything that paints a slice letter
            // — the active slice path's syncFromSlice() pulls.  Cheapest
            // broad-stroke: re-emit currentSliceChanged so the model
            // listeners reapply via their existing slots.
            emit sliceLetterDisplayModeChanged();
        };
        connect(globalRadio, &QRadioButton::toggled, this, [saveLetterMode](bool on) {
            if (on) saveLetterMode("Global");
        });
        connect(radioIdxRadio, &QRadioButton::toggled, this, [saveLetterMode](bool on) {
            if (on) saveLetterMode("RadioIndexed");
        });

        vbox->addWidget(letterGrp);
    }

    // ── Slice color group ────────────────────────────────────────────────────
    auto* grp = new QGroupBox("Slice Colors");
    auto* grpLayout = new QVBoxLayout(grp);
    grpLayout->setSpacing(10);

    auto* modeLayout = new QHBoxLayout;
    auto* defaultsRadio = new QRadioButton("Use Aether defaults");
    auto* customRadio   = new QRadioButton("Custom colors");
    modeLayout->addWidget(defaultsRadio);
    modeLayout->addWidget(customRadio);
    modeLayout->addStretch();
    grpLayout->addLayout(modeLayout);

    auto* desc = new QLabel(
        "Customize the color used for each slice marker, filter band, and badge.");
    setTone(desc, Tone::Hint);
    desc->setWordWrap(true);
    grpLayout->addWidget(desc);

    // Grid of 8 color buttons (A–H)
    auto* colorGrid = new QHBoxLayout;
    colorGrid->setSpacing(8);

    static const char kLetters[] = "ABCDEFGH";
    // One button per slice; holds the current color as its background.
    QVector<QPushButton*> colorBtns;
    for (int i = 0; i < AetherSDR::kSliceColorCount; ++i) {
        auto* col = new QVBoxLayout;
        col->setSpacing(4);

        // The swatch carries its own letter; no caption above it.
        auto* btn = new QPushButton(QString(kLetters[i]));
        ThemeManager::instance().applyStyleSheet(btn, kBtnBase);
        colorBtns.append(btn);

        col->addWidget(btn);
        colorGrid->addLayout(col);
    }
    colorGrid->addStretch();
    grpLayout->addLayout(colorGrid);

    // Reset-all button
    auto* resetRow = new QHBoxLayout;
    auto* resetBtn = new QPushButton("Reset All to Defaults");
    resetRow->addWidget(resetBtn);
    resetRow->addStretch();
    grpLayout->addLayout(resetRow);

    vbox->addWidget(grp);

    // ── Helpers (capture manager by pointer, buttons by value) ────────────────
    SliceColorManager* pMgr = &SliceColorManager::instance();

    // Updates a single button's background to reflect current color.
    auto applyBtnColor = [pMgr, colorBtns](int idx) mutable {
        QColor c = pMgr->activeColor(idx);
        bool light = (c.red() * 299 + c.green() * 587 + c.blue() * 114) > 128000;
        QString textColor = light ? "#000000" : "#ffffff";
        ThemeManager::instance().applyStyleSheet(colorBtns[idx],
            kBtnBase +
            QStringLiteral("QPushButton { background: %1; color: %2; }")
                .arg(c.name(), textColor));
    };

    auto refreshAllBtns = [applyBtnColor]() mutable {
        for (int i = 0; i < AetherSDR::kSliceColorCount; ++i)
            applyBtnColor(i);
    };

    auto syncModeRadios = [defaultsRadio, customRadio, colorBtns](bool custom) mutable {
        defaultsRadio->blockSignals(true);
        customRadio->blockSignals(true);
        defaultsRadio->setChecked(!custom);
        customRadio->setChecked(custom);
        defaultsRadio->blockSignals(false);
        customRadio->blockSignals(false);
        for (QPushButton* b : colorBtns)
            b->setEnabled(custom);
    };

    // ── Initial state ─────────────────────────────────────────────────────────
    syncModeRadios(pMgr->useCustomColors());
    refreshAllBtns();

    // ── Signals ───────────────────────────────────────────────────────────────
    connect(defaultsRadio, &QRadioButton::toggled, page,
            [pMgr, syncModeRadios, refreshAllBtns](bool checked) mutable {
        if (!checked) return;
        pMgr->setUseCustomColors(false);
        syncModeRadios(false);
        refreshAllBtns();
    });

    connect(customRadio, &QRadioButton::toggled, page,
            [pMgr, syncModeRadios, refreshAllBtns](bool checked) mutable {
        if (!checked) return;
        pMgr->setUseCustomColors(true);
        syncModeRadios(true);
        refreshAllBtns();
    });

    for (int i = 0; i < AetherSDR::kSliceColorCount; ++i) {
        connect(colorBtns[i], &QPushButton::clicked, page,
                [i, pMgr, applyBtnColor, page]() mutable {
            const QPointer<QWidget> pageGuard(page);
            const QPointer<SliceColorManager> mgr(pMgr);
            QColor initial = pMgr->customColor(i);
            QColor chosen = QColorDialog::getColor(initial, page,
                                                   QStringLiteral("Slice %1 Color")
                                                       .arg(QChar('A' + i)));
            if (!pageGuard || !mgr || !chosen.isValid()) {
                return;
            }
            mgr->setCustomColor(i, chosen);
            applyBtnColor(i);
        });
    }

    connect(resetBtn, &QPushButton::clicked, page,
            [pMgr, refreshAllBtns]() mutable {
        for (int i = 0; i < AetherSDR::kSliceColorCount; ++i)
            pMgr->resetToDefault(i);
        refreshAllBtns();
    });

    // Sync button backgrounds when another widget changes a color
    connect(pMgr, &SliceColorManager::colorsChanged, page,
            [syncModeRadios, refreshAllBtns, pMgr]() mutable {
        syncModeRadios(pMgr->useCustomColors());
        refreshAllBtns();
    });

    // ── Single-click delay group (#3009) ────────────────────────────────────
    // Several widgets (slice-mute button, RX chain stages, waveform widgets)
    // defer the single-click action by this interval so a double click can
    // override it.  Default is the platform's QApplication::doubleClickInterval
    // (typically 400 ms on Linux, 500 ms on Windows).  Power users who never
    // double-click can set this to 0 for instant single-click response;
    // double-click affordances become unreachable in that case but the
    // mute-all keyboard shortcut still works for the most common use.
    {
        auto* clickGrp = new QGroupBox("Single-Click Delay");
        auto* clickLayout = new QVBoxLayout(clickGrp);
        clickLayout->setSpacing(8);

        auto* row = new QHBoxLayout;
        row->setSpacing(8);

        auto* clickLabel = new QLabel("Delay (ms):");
        setTone(clickLabel, Tone::Caption);
        row->addWidget(clickLabel);

        auto& s = AppSettings::instance();
        const int platformDefault = QApplication::doubleClickInterval();
        const int current = s.value("ClickDiscriminationIntervalMs",
                                     platformDefault).toInt();

        auto* clickSpin = new QSpinBox;
        clickSpin->setRange(0, 1000);
        clickSpin->setSingleStep(50);
        clickSpin->setSuffix(" ms");
        clickSpin->setValue(qBound(0, current, 1000));
        clickSpin->setFixedWidth(110);
        row->addWidget(clickSpin);

        auto* resetBtn = new QPushButton("Reset");
        resetBtn->setToolTip(QString("Reset to platform default (%1 ms)")
                             .arg(platformDefault));
        row->addWidget(resetBtn);

        row->addStretch();
        clickLayout->addLayout(row);

        auto* clickDesc = new QLabel(QString(
            "Time AetherSDR waits after a single click on a widget that "
            "also has a double-click action (e.g. the slice mute button) "
            "before firing the single-click. The platform default is "
            "%1 ms. Set to 0 to fire single-click actions instantly — "
            "this also disables the double-click affordances on those "
            "widgets.").arg(platformDefault));
        setTone(clickDesc, Tone::Hint);
        clickDesc->setWordWrap(true);
        clickLayout->addWidget(clickDesc);

        connect(clickSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [](int v) {
            auto& s = AppSettings::instance();
            s.setValue("ClickDiscriminationIntervalMs", QString::number(v));
            s.save();
        });
        connect(resetBtn, &QPushButton::clicked, this,
                [clickSpin, platformDefault]() {
            clickSpin->setValue(platformDefault);
            // valueChanged handler above persists the value.
        });

        vbox->addWidget(clickGrp);
    }

    // ── Mouse wheel group (#3302) ───────────────────────────────────────────
    // Trackball / inverted-scroll users (and parity with Thetis / KE9NS) get
    // a single checkbox that reverses the wheel direction for frequency
    // tuning.  Reading is per-event in the wheel handlers, so this takes
    // effect immediately with no signal plumbing.  The Ctrl+wheel bandwidth
    // zoom on the panadapter is intentionally not reversed.
    {
        auto* wheelGrp = new QGroupBox("Mouse Wheel");
        auto* wheelLayout = new QVBoxLayout(wheelGrp);
        wheelLayout->setSpacing(8);

        auto* reverseChk = new QCheckBox("Reverse mouse-wheel tuning direction");
        {
            auto& s = AppSettings::instance();
            reverseChk->setChecked(s.value("ReverseMouseWheel", false).toBool());
        }
        wheelLayout->addWidget(reverseChk);

        auto* wheelDesc = new QLabel(
            "When enabled, scrolling the wheel up tunes the frequency down "
            "(and vice versa). Useful for trackballs and any pointer where "
            "the natural scroll direction feels inverted. Affects the VFO "
            "frequency display and the panadapter / waterfall; the "
            "Ctrl+wheel bandwidth zoom is not reversed.");
        setTone(wheelDesc, Tone::Hint);
        wheelDesc->setWordWrap(true);
        wheelLayout->addWidget(wheelDesc);

        connect(reverseChk, &QCheckBox::toggled, this, [](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("ReverseMouseWheel", on);
            s.save();
        });

        vbox->addWidget(wheelGrp);
    }

    // The page's slack goes under its last group, not between two of them.
    vbox->addStretch();
    return page;
}

QWidget* RadioSetupDialog::buildSmartLinkTab()
{
    // Phase 2 of GHSA-wfx7-w6p8-4jr2 (#2951): Pinned Certificates panel.
    // Lists every SmartLink host this client has TOFU-pinned a cert
    // fingerprint for, when it was pinned, and lets the operator
    // forget individual pins or clear the whole cache. Used when the
    // operator deliberately rotates a radio's cert (firmware update,
    // hardware replacement) and wants the next connect to re-pin
    // silently instead of triggering the mismatch dialog.

    auto* page = new QWidget;
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto* grp = new QGroupBox("Pinned SmartLink Certificates");
    auto* grpLay = new QVBoxLayout(grp);
    grpLay->setSpacing(8);

    auto* desc = new QLabel(
        "AetherSDR pins each SmartLink radio's TLS certificate the first "
        "time you connect (trust-on-first-use). Subsequent connects "
        "compare against the pin; a mismatch shows a warning dialog and "
        "pauses the connection until you accept or reject it.\n\n"
        "Forget a pin when you deliberately change a radio's certificate "
        "(firmware update, hardware replacement) so the next connect can "
        "re-pin silently. See GHSA-wfx7-w6p8-4jr2 for the threat model.");
    setTone(desc, Tone::Hint);
    desc->setWordWrap(true);
    grpLay->addWidget(desc);

    auto* table = new QTableWidget(0, 3, grp);
    m_pinnedCertsTable = table;
    table->setHorizontalHeaderLabels({"Host", "SHA-256 Fingerprint", "Pinned"});
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);
    // Headings sit over the start of their column, as the cells do.
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    grpLay->addWidget(table);

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    auto* forgetSel = new QPushButton("Forget Selected");
    auto* forgetAll = new QPushButton("Forget All");
    btnRow->addWidget(forgetSel);
    btnRow->addStretch();
    btnRow->addWidget(forgetAll);
    grpLay->addLayout(btnRow);

    connect(forgetSel, &QPushButton::clicked, this, [this, table]() {
        const int row = table->currentRow();
        if (row < 0) return;
        auto* item = table->item(row, 0);
        if (!item) return;
        const QString host = item->text();
        WanCertCache::forgetPinnedCert(host);
        refreshPinnedCertsTable();
    });

    connect(forgetAll, &QPushButton::clicked, this, [this]() {
        const QPointer<RadioSetupDialog> self(this);
        ScopedChildWidget<QMessageBox> boxOwner(
            QMessageBox::Question, tr("Forget all SmartLink certificates"),
            tr("Clear every pinned SmartLink cert fingerprint?\n\n"
               "Next connect to each radio will silently re-pin "
               "whatever certificate it presents (no mismatch warning)."),
            QMessageBox::Yes | QMessageBox::No, this);
        const int reply = boxOwner.get()->exec();
        if (!self || !boxOwner || reply != QMessageBox::Yes) {
            return;
        }
        WanCertCache::forgetAllPinnedCerts();
        self->refreshPinnedCertsTable();
    });

    root->addWidget(grp);
    root->addStretch();

    refreshPinnedCertsTable();
    return page;
}

void RadioSetupDialog::refreshPinnedCertsTable()
{
    if (!m_pinnedCertsTable) return;
    const QVector<PinnedCertInfo> pins = WanCertCache::listPinnedCerts();
    m_pinnedCertsTable->setRowCount(pins.size());
    for (int i = 0; i < pins.size(); ++i) {
        const PinnedCertInfo& pin = pins.at(i);
        m_pinnedCertsTable->setItem(i, 0, new QTableWidgetItem(pin.host));
        // Use a monospace cell for the fingerprint so the hex aligns.
        auto* fpItem = new QTableWidgetItem(pin.fingerprintHex);
        QFont mono("monospace");
        fpItem->setFont(mono);
        fpItem->setToolTip(pin.fingerprintHex);
        m_pinnedCertsTable->setItem(i, 1, fpItem);
        const QString when = pin.pinnedAtIso.isEmpty()
            ? QStringLiteral("(pre-phase 2)")
            : pin.pinnedAtIso.left(10);   // YYYY-MM-DD
        m_pinnedCertsTable->setItem(i, 2, new QTableWidgetItem(when));
    }
}

QWidget* RadioSetupDialog::buildQrzTab()
{
    auto* page = new QWidget;
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto& svc = CallsignLookupService::instance();

    // ---- Account group -------------------------------------------------
    auto* acct = new QGroupBox("QRZ.com Account");
    auto* acctLay = new QVBoxLayout(acct);
    acctLay->setSpacing(8);

    auto* desc = new QLabel(
        "AetherSDR uses your QRZ.com account to look up station details — "
        "name, location, grid, and photo — for callsigns heard in the CW "
        "decoder and entered in View → Callsign Lookup. Enable View → Show live CW "
        "contacts to see decoded stations in a separate window. An XML Logbook Data "
        "subscription returns full details; a free account returns limited "
        "fields. Your password is stored in the operating system keychain, "
        "never in the settings file.");
    setTone(desc, Tone::Hint);
    desc->setWordWrap(true);
    acctLay->addWidget(desc);

    auto* enableCheck = new QCheckBox("Enable QRZ callsign lookups");
    enableCheck->setObjectName("qrzEnableCheck");
    enableCheck->setAccessibleName("Enable QRZ callsign lookups");
    enableCheck->setChecked(QrzLookupSettings::enabled());
    connect(enableCheck, &QCheckBox::toggled, this, [](bool on) {
        QrzLookupSettings::setEnabled(on);
        CallsignLookupService::instance().reloadConfiguration();
    });
    acctLay->addWidget(enableCheck);

    auto* grid = new QGridLayout;
    grid->setSpacing(8);

    auto* userLbl = new QLabel("Username (callsign):");
    applyLabelStyle(userLbl);
    grid->addWidget(userLbl, 0, 0);
    auto* userEdit = new QLineEdit(QrzLookupSettings::username());
    userEdit->setObjectName("qrzUsernameEdit");
    userEdit->setAccessibleName("QRZ username");
    userEdit->setMaxLength(64);
    grid->addWidget(userEdit, 0, 1);

    auto* passLbl = new QLabel("Password:");
    applyLabelStyle(passLbl);
    grid->addWidget(passLbl, 1, 0);
    auto* passEdit = new QLineEdit;
    passEdit->setObjectName("qrzPasswordEdit");
    passEdit->setAccessibleName("QRZ password");
    passEdit->setEchoMode(QLineEdit::Password);
    passEdit->setMaxLength(128);
    grid->addWidget(passEdit, 1, 1);
    // A callsign and a password: fields sized for them, not the page.
    userEdit->setFixedWidth(260);
    passEdit->setFixedWidth(260);
    grid->setColumnStretch(2, 1);
    acctLay->addLayout(grid);

    // Populate the password field from the keychain (async).
    QPointer<QLineEdit> passGuard(passEdit);
    auto passLoaded = std::make_shared<bool>(false);
    svc.readPassword([passGuard, passLoaded](const QString& pw) {
        *passLoaded = true;
        if (passGuard && passGuard->text().isEmpty())
            passGuard->setText(pw);
    });

    connect(userEdit, &QLineEdit::editingFinished, this, [userEdit] {
        QrzLookupSettings::setUsername(userEdit->text().trimmed());
        CallsignLookupService::instance().reloadConfiguration();
    });
    connect(passEdit, &QLineEdit::editingFinished, this, [passEdit, passLoaded] {
        // Don't let a focus-out before the async keychain read completes delete
        // a stored password: skip the save when the field is empty and the read
        // hasn't landed yet (savePassword("") deletes the keychain entry). (#3990)
        if (!*passLoaded && passEdit->text().isEmpty())
            return;
        CallsignLookupService::instance().savePassword(passEdit->text());
    });

    auto* testRow = new QHBoxLayout;
    testRow->setSpacing(8);
    auto* testBtn = new QPushButton("Test Login");
    testBtn->setObjectName("qrzTestLoginBtn");
    testBtn->setAccessibleName("Test QRZ login");
    testRow->addWidget(testBtn);
    auto* testStatus = new QLabel;
    testStatus->setObjectName("qrzTestStatus");
    setTone(testStatus, Tone::Hint);
    testStatus->setWordWrap(true);
    testRow->addWidget(testStatus, 1);
    // Under the fields it tests, not under their captions.
    grid->addLayout(testRow, 2, 1, 1, 2);

    connect(testBtn, &QPushButton::clicked, this,
            [testBtn, testStatus, userEdit, passEdit] {
        const QString user = userEdit->text().trimmed();
        const QString pass = passEdit->text();
        if (user.isEmpty() || pass.isEmpty()) {
            testStatus->setText("Enter a username and password first.");
            return;
        }
        testBtn->setEnabled(false);
        testStatus->setText("Contacting QRZ.com…");
        CallsignLookupService::instance().testLogin(user, pass);
    });
    connect(&svc, &CallsignLookupService::loginTestFinished, this,
            [testBtn, testStatus](bool ok, const QString& message) {
        testBtn->setEnabled(true);
        testStatus->setText(ok
            ? (message.isEmpty() ? QStringLiteral("Login OK.")
                                 : QStringLiteral("Login OK — %1").arg(message))
            : QStringLiteral("Login failed: %1").arg(message));
        setTone(testStatus, ok ? Tone::Ok : Tone::Error);
    });

    root->addWidget(acct);

    // ---- Cache group ----------------------------------------------------
    auto* cache = new QGroupBox("Lookup Cache");
    auto* cacheLay = new QVBoxLayout(cache);
    cacheLay->setSpacing(8);

    auto* cacheDesc = new QLabel(
        "Looked-up callsigns are cached for 7 days so a busy net never asks "
        "QRZ twice for the same station. Station photos are cached alongside.");
    setTone(cacheDesc, Tone::Hint);
    cacheDesc->setWordWrap(true);
    cacheLay->addWidget(cacheDesc);

    auto* cacheRow = new QHBoxLayout;
    cacheRow->setSpacing(8);
    auto* cacheCount = makeValueLabel(QString());
    cacheCount->setObjectName("qrzCacheCount");
    auto refreshCacheCount = [cacheCount] {
        cacheCount->setText(QStringLiteral("%1 cached callsign(s)")
            .arg(CallsignLookupService::instance().cacheEntryCount()));
    };
    refreshCacheCount();
    cacheRow->addWidget(cacheCount);
    cacheRow->addStretch();

    auto* clearBtn = new QPushButton("Clear Cache");
    clearBtn->setObjectName("qrzClearCacheBtn");
    clearBtn->setAccessibleName("Clear QRZ lookup cache");
    connect(clearBtn, &QPushButton::clicked, this, [refreshCacheCount] {
        CallsignLookupService::instance().clearCache();
        refreshCacheCount();
    });
    cacheRow->addWidget(clearBtn);
    cacheLay->addLayout(cacheRow);

    root->addWidget(cache);
    root->addStretch();
    return page;
}

} // namespace AetherSDR
