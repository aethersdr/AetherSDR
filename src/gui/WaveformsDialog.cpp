#include "WaveformsDialog.h"
#include "core/DigitalVoiceWaveformProcess.h"
#include "core/DigitalVoiceWaveformSettings.h"
#include "core/DigitalVoiceFeature.h"
#include "core/ThemeManager.h"
#include "core/WaveformInstaller.h"
#include "models/FlexWaveformModel.h"
#include "models/RadioModel.h"
#include "gui/WaveformInstallGate.h"   // #4210 pure Docker-install gate policy

#include <QAction>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

constexpr int kDStarControlWidth = 92;
constexpr int kDStarStatusHeight = 28;
constexpr int kDStarButtonHeight = 30;
constexpr int kDStarControlSpacing = 6;
constexpr int kDStarServiceCardMinHeight = 210;
constexpr int kWaveformInputMinHeight = 34;
constexpr int kLocalServicesScrollbarGutter = 14;
constexpr int kInstalledWaveformActionButtonMinWidth = 96;
constexpr int kInstalledWaveformActionButtonSpacing = 8;
constexpr int kInstalledWaveformTypeBadgeMinWidth = 72;
constexpr int kInstalledWaveformTypeBadgeHorizontalPadding = 24;
// Local digital-voice services are configured in AetherModem. Keep this false
// while the legacy construction code is retained for a follow-up cleanup.
constexpr bool kShowLocalDigitalVoiceControls = false;

struct WfpSupportUiState {
    QString label;
    QString hint;
    bool supported{false};
};

// isKnownNonWfpPlatform() and the DockerWaveformInstallBlocker gate policy live
// in gui/WaveformInstallGate.h so they're unit-testable (#4210).

WfpSupportUiState wfpSupportUiState(const RadioModel* radioModel)
{
    if (!radioModel || !radioModel->isConnected()) {
        return {
            QObject::tr("No radio"),
            QObject::tr("Connect to a radio to query Waveform Processor support."),
            false
        };
    }

    // Reflect the radio's actual WFP hardware capability, NOT the FlexLib "wfp"
    // license feature (#4210). That feature is a SmartSDR+/EA-style entitlement
    // decoupled from whether the Waveform Processor works — a radio with WFP
    // powered and ready can report it disabled — so it must not drive this pill
    // any more than it gates the install action (the two would otherwise
    // contradict: "Unsupported" next to an enabled Install). Platforms with no
    // WFP hardware (6000-series Microburst/DeepEddy) are genuinely unsupported;
    // every other platform has the Waveform Processor, whose live power/ready
    // state is shown by the separate WFP Power / WFP Ready pills.
    const RadioPlatform platform = radioModel->capabilities().platform;
    if (platform == RadioPlatform::Unknown) {
        return {
            QObject::tr("Checking"),
            QObject::tr("Waiting for the radio to identify its Waveform Processor support."),
            false
        };
    }
    if (isKnownNonWfpPlatform(platform)) {
        return {
            QObject::tr("Not supported"),
            QObject::tr("This 6000-series radio platform does not support on-radio Docker waveform deployment."),
            false
        };
    }

    return {
        QObject::tr("Supported"),
        QObject::tr("This radio has a Waveform Processor for Docker waveform images."),
        true
    };
}

QString wfpRuntimeStatusText(const FlexWaveformModel& wfModel)
{
    const QString powerText = wfModel.wfpPowered()
        ? QObject::tr("WFP ON")
        : QObject::tr("WFP OFF");
    const QString readyText = wfModel.wfpReady()
        ? QObject::tr("READY")
        : QObject::tr("NOT READY");
    return QObject::tr("%1, %2").arg(powerText, readyText);
}

QString dockerInstallBlockerText(const RadioModel* radioModel,
                                 const FlexWaveformModel& wfModel)
{
    // Gate on the radio's live WFP runtime state plus the genuine no-WFP-hardware
    // platform check only — not the "wfp" license feature (#4210; the policy and
    // its rationale live in WaveformInstallGate.h). wfpSupportUiState()/the "WFP
    // Support" pill stay informational and do not gate the action. This maps the
    // pure gate result to the user-facing message.
    const bool connected = radioModel && radioModel->isConnected();
    const RadioPlatform platform = radioModel
        ? radioModel->capabilities().platform
        : RadioPlatform::Unknown;
    switch (dockerWaveformInstallBlocker(connected, platform,
                                         wfModel.wfpStatusSeen(),
                                         wfModel.wfpPowered(),
                                         wfModel.wfpReady())) {
    case DockerWaveformInstallBlocker::None:
        return {};
    case DockerWaveformInstallBlocker::NotConnected:
        return QObject::tr(
            "Connect to a radio before installing Docker waveform images.");
    case DockerWaveformInstallBlocker::UnsupportedPlatform:
        return QObject::tr(
            "This radio platform does not support on-radio Docker waveform "
            "deployment.");
    case DockerWaveformInstallBlocker::RuntimeStatusUnknown:
        return QObject::tr(
            "WFP runtime status has not been reported by this radio.");
    case DockerWaveformInstallBlocker::WfpNotReady:
        return wfpRuntimeStatusText(wfModel);
    }
    return {};
}

QString existingParentDirectoryForPath(const QFileInfo& info)
{
    const QString parentPath = info.absolutePath();
    if (!parentPath.isEmpty() && QDir(parentPath).exists()) {
        return parentPath;
    }
    return {};
}

QString dstarExecutableBrowseStartPath(const QString& configuredPath)
{
    const QString executable =
        DigitalVoiceWaveformProcess::resolveExecutablePath(configuredPath);
    QFileInfo info(executable);

    if (!info.isAbsolute() && info.path() == QLatin1String(".")) {
        const QString fromPath = QStandardPaths::findExecutable(executable);
        if (!fromPath.isEmpty()) {
            info.setFile(fromPath);
        }
    }

    if (info.exists()) {
        return info.absoluteFilePath();
    }

    if (info.isAbsolute() || info.path() != QLatin1String(".")) {
        const QString configuredParent = existingParentDirectoryForPath(info);
        if (!configuredParent.isEmpty()) {
            return configuredParent;
        }
    }

    const QFileInfo defaultInfo(DigitalVoiceWaveformProcess::defaultExecutablePath());
    if (defaultInfo.exists()) {
        return defaultInfo.absoluteFilePath();
    }

    const QString defaultParent = existingParentDirectoryForPath(defaultInfo);
    if (!defaultParent.isEmpty()) {
        return defaultParent;
    }

    return QDir::homePath();
}

QString legacyWaveformDialogInitialPath()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates {
        appDir.filePath(QStringLiteral("waveforms")),
        appDir.filePath(QStringLiteral("../Resources/waveforms")),
        appDir.filePath(QStringLiteral("../share/AetherSDR/waveforms")),
        appDir.filePath(QStringLiteral("../share/aethersdr/waveforms"))
    };
    for (const QString& candidate : candidates) {
        const QDir dir(candidate);
        if (dir.exists()) {
            return dir.absolutePath();
        }
    }
    return QDir::homePath();
}

constexpr const char* kWaveformsDialogStyle = R"(
QWidget#waveformsBody {
    color: #aeb9cc;
    background: #07101c;
    font-size: 14px;
}
QWidget#StatusColumn,
QWidget#LocalServicesList,
QWidget#LocalServicesScrollContent,
QWidget#LocalServiceBody,
QWidget#DStarSerialRow,
QWidget#DStarAdvancedPanel,
QWidget#DStarExecutableRow {
    background: transparent;
}
QLabel {
    background: transparent;
}
QFrame#WaveformsStatusFrame,
QFrame#localDigitalVoicePanel,
QFrame#installedWaveformsPanel {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #111d2c, stop:1 #0a1421);
    border: 1px solid #233246;
    border-radius: 7px;
}
QFrame#LocalWaveformServiceCard {
    background: #0b1625;
    border-color: #26374e;
    border: 1px solid #26374e;
    border-radius: 7px;
}
QFrame#ActiveServiceStrip {
    background: #8294a8;
    border: none;
    border-radius: 2px;
    min-width: 4px;
    max-width: 4px;
}
QFrame#PanelSeparator {
    background: #26374e;
    border: none;
    min-height: 1px;
    max-height: 1px;
}
QFrame#StatusSegmentDivider {
    background: #26374e;
    border: none;
    min-width: 1px;
    max-width: 1px;
}
QLabel#StatusStripTitle,
QLabel#SectionLabel,
QLabel#StatusColumnTitle {
    color: #8d99ad;
    font-size: 11px;
    font-weight: 700;
}
QLabel#PanelTitle {
    color: #d4deea;
    font-size: 15px;
    font-weight: 700;
}
QLabel#ConnectedRadioName {
    color: #e1e8f1;
    font-size: 15px;
    font-weight: 700;
}
QLabel#ConnectedRadioSerial,
QLabel#ServiceSubtitle,
QLabel#EmptyStateSubtext {
    color: #8d99ad;
}
QLabel#ServiceSubtitle {
    min-height: 18px;
}
QLabel#digitalVoiceWaveformDetail {
    color: #9fb0c6;
    font-size: 12px;
}
QLabel#ServiceTitle {
    color: #e1e8f1;
    font-size: 15px;
    font-weight: 700;
}
QLabel#StatusPill,
QLabel#digitalVoiceWaveformStatus,
QLabel#ValuePill {
    color: #c8d8e8;
    background: #0b1625;
    border: 1px solid #26374e;
    border-radius: 5px;
    padding: 4px 9px;
    font-weight: 600;
}
QLabel#CapabilityPill {
    color: #c8d8e8;
    background: #0b1625;
    border: 1px solid #26374e;
    border-radius: 5px;
    padding: 4px 9px;
    font-weight: 600;
}
QLabel#ValuePill {
    color: #d4deea;
    background: #0c1b28;
}
QLabel#MutedLabel,
QLabel#EmptyStateText {
    color: #8d99ad;
}
QLabel#EmptyStateText {
    color: #9bb0c4;
    font-size: 14px;
    font-weight: 700;
}
QLabel#EmptyStateIcon {
    background: transparent;
    border: none;
    min-width: 56px;
    max-width: 56px;
    min-height: 56px;
    max-height: 56px;
}
QLabel#ProtocolIcon {
    background: transparent;
    border: none;
    min-width: 56px;
    max-width: 56px;
    min-height: 56px;
    max-height: 56px;
}
QFrame#WaveformListFrame {
    background: #07101c;
    border: 1px dashed #31455c;
    border-radius: 7px;
}
QFrame#RadioWaveformRow {
    background: #0b1625;
    border: 1px solid #1d2a3c;
    border-radius: 6px;
}
QLabel#WaveformTypeBadge {
    color: #9ab2c8;
    background: #0d1c20;
    border: 1px solid #26374e;
    border-radius: 5px;
    padding: 3px 8px;
    font-size: 12px;
    font-weight: 600;
}
QCheckBox {
    background: transparent;
    color: #aeb9cc;
    spacing: 9px;
}
QCheckBox::indicator {
    width: 20px;
    height: 20px;
    border-radius: 4px;
    border: 1px solid #34533c;
    background: #0d1a18;
}
QCheckBox::indicator:checked {
    background: #5ebd69;
    border-color: #65d379;
}
QCheckBox::indicator:disabled {
    border-color: #26374e;
    background: #08111d;
}
QPushButton,
QToolButton {
    color: #c8d8e8;
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #17314a, stop:1 #0d2134);
    border: 1px solid #2d4d66;
    border-radius: 6px;
    padding: 7px 14px;
    font-weight: 600;
}
QPushButton:hover,
QToolButton:hover {
    border-color: #4f7390;
    color: #e2edf7;
}
QPushButton:disabled,
QToolButton:disabled {
    color: #68778a;
    border-color: #1d2a3c;
    background: #0b1522;
}
QToolButton#DStarAdvancedButton {
    color: #aeb9cc;
    background: transparent;
    border: 1px solid transparent;
    padding: 3px 6px;
    text-align: left;
}
QToolButton#DStarAdvancedButton:hover {
    border-color: #26374e;
    background: #0b1625;
}
QToolButton#ThumbDvDeviceMenu {
    padding: 0;
}
QComboBox {
    color: #c4cedd;
    background: #0b1625;
    border: 1px solid #26374e;
    border-radius: 5px;
    padding: 6px 28px 6px 10px;
    selection-background-color: #1b3650;
}
QComboBox:focus {
    border-color: #54c768;
}
QLineEdit {
    color: #c4cedd;
    background: #050b13;
    border: 1px solid #26374e;
    border-radius: 6px;
    padding: 6px 10px;
    min-height: 20px;
    selection-background-color: #1b3650;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: 13px;
}
QLineEdit:focus {
    border-color: #54c768;
}
QScrollArea {
    background: transparent;
    border: none;
}
QScrollBar:vertical {
    background: #07101c;
    width: 12px;
    margin: 8px 2px 8px 2px;
    border-radius: 6px;
}
QScrollBar::handle:vertical {
    background: #25364d;
    border-radius: 5px;
    min-height: 34px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0px;
}
)";

constexpr const char* kWaveformsInstallMenuStyle = R"(
QMenu {
    color: #c8d8e8;
    background: #07101c;
    border: 1px solid #26374e;
}
QMenu::item {
    padding: 6px 28px 6px 14px;
}
QMenu::item:selected:enabled {
    color: #e2edf7;
    background: #17314a;
}
QMenu::item:disabled {
    color: #596779;
}
)";

[[maybe_unused]] void applyComboHeight(QComboBox* combo)
{
    const int minHeight = qMax(combo->sizeHint().height(),
                               combo->fontMetrics().height() + 12);
    combo->setMinimumHeight(minHeight);
}

QFrame* makePanel(const QString& objectName, QWidget* parent = nullptr)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(objectName);
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setAttribute(Qt::WA_StyledBackground, true);
    return frame;
}

QLabel* makePanelTitle(const QString& text, QWidget* parent = nullptr)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("PanelTitle"));
    return label;
}

[[maybe_unused]] QLabel* makeServiceTitle(const QString& text, QWidget* parent = nullptr)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("ServiceTitle"));
    return label;
}

QLabel* makeMutedLabel(const QString& text, QWidget* parent = nullptr)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("MutedLabel"));
    return label;
}

QFrame* makeSeparator(QWidget* parent = nullptr)
{
    auto* separator = new QFrame(parent);
    separator->setObjectName(QStringLiteral("PanelSeparator"));
    separator->setFrameShape(QFrame::NoFrame);
    separator->setAttribute(Qt::WA_StyledBackground, true);
    return separator;
}

QFrame* makeStatusDivider(QWidget* parent = nullptr)
{
    auto* divider = new QFrame(parent);
    divider->setObjectName(QStringLiteral("StatusSegmentDivider"));
    divider->setFrameShape(QFrame::NoFrame);
    divider->setAttribute(Qt::WA_StyledBackground, true);
    return divider;
}

QLabel* makeStatusPill(const QString& accessibleName, QWidget* parent = nullptr)
{
    auto* pill = new QLabel(parent);
    pill->setObjectName(QStringLiteral("StatusPill"));
    pill->setTextFormat(Qt::RichText);
    pill->setAccessibleName(accessibleName);
    pill->setAlignment(Qt::AlignCenter);
    pill->setMinimumHeight(28);
    pill->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    return pill;
}

QWidget* makeStatusColumn(const QString& title, QLabel* valueLabel, QWidget* parent = nullptr)
{
    auto* column = new QWidget(parent);
    column->setObjectName(QStringLiteral("StatusColumn"));
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* titleLabel = new QLabel(title, column);
    titleLabel->setObjectName(QStringLiteral("StatusColumnTitle"));
    layout->addWidget(titleLabel);
    layout->addWidget(valueLabel);
    return column;
}

QPixmap makeProtocolIconPixmap(const QString& label, bool enabled)
{
    constexpr int kSize = 56;
    QPixmap pixmap(kSize, kSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QRectF bounds(0.5, 0.5, kSize - 1.0, kSize - 1.0);
    QPainterPath clipPath;
    clipPath.addRoundedRect(bounds, 10.0, 10.0);

    painter.fillPath(clipPath, QColor(enabled ? QStringLiteral("#071321")
                                              : QStringLiteral("#262b31")));
    painter.save();
    painter.setClipPath(clipPath);

    const QPixmap logo(QStringLiteral(":/icon.png"));
    if (enabled && !logo.isNull()) {
        const QPixmap scaledLogo = logo.scaled(kSize, kSize,
                                               Qt::KeepAspectRatioByExpanding,
                                               Qt::SmoothTransformation);
        const QRect targetRect(0, 0, kSize, kSize);
        const QRect sourceRect((scaledLogo.width() - kSize) / 2,
                               (scaledLogo.height() - kSize) / 2,
                               kSize,
                               kSize);
        painter.setOpacity(0.34);
        painter.drawPixmap(targetRect, scaledLogo, sourceRect);
        painter.setOpacity(1.0);
    } else {
        QFont fallbackFont = painter.font();
        fallbackFont.setBold(true);
        fallbackFont.setPointSize(28);
        painter.setFont(fallbackFont);
        painter.setOpacity(enabled ? 1.0 : 0.22);
        painter.setPen(QColor(enabled ? QStringLiteral("#6fcde8")
                                      : QStringLiteral("#a5adb5")));
        painter.drawText(bounds, Qt::AlignCenter, QStringLiteral("A"));
        painter.setOpacity(1.0);
    }

    painter.fillRect(pixmap.rect(),
                     QColor(enabled ? 5 : 29,
                            enabled ? 12 : 32,
                            enabled ? 22 : 36,
                            enabled ? 88 : 70));
    painter.restore();

    painter.setPen(QPen(QColor(enabled ? QStringLiteral("#31455c")
                                      : QStringLiteral("#454d55")),
                        1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(clipPath);

    QFont labelFont = painter.font();
    labelFont.setBold(true);
    labelFont.setPointSize(label.size() <= 3 ? 12 : 9);
    QFontMetrics metrics(labelFont);
    while (metrics.horizontalAdvance(label) > kSize - 8 && labelFont.pointSize() > 7) {
        labelFont.setPointSize(labelFont.pointSize() - 1);
        metrics = QFontMetrics(labelFont);
    }

    painter.setFont(labelFont);
    const QRect textRect = pixmap.rect().adjusted(3, 0, -3, 0);
    painter.setPen(QColor(0, 0, 0, enabled ? 185 : 120));
    painter.drawText(textRect.translated(0, 1), Qt::AlignCenter, label);
    painter.setPen(QColor(enabled ? QStringLiteral("#eef8ff")
                                  : QStringLiteral("#8f989f")));
    painter.drawText(textRect, Qt::AlignCenter, label);

    return pixmap;
}

[[maybe_unused]] QLabel* makeProtocolIconLabel(const QString& protocol, bool enabled, QWidget* parent = nullptr)
{
    auto* label = new QLabel(parent);
    label->setObjectName(QStringLiteral("ProtocolIcon"));
    label->setPixmap(makeProtocolIconPixmap(protocol, enabled));
    label->setAlignment(Qt::AlignCenter);
    label->setEnabled(enabled);
    label->setAccessibleName(QObject::tr("%1 waveform icon").arg(protocol));
    return label;
}

QPixmap makeWaveformDocumentPixmap()
{
    QPixmap pixmap(56, 56);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor borderColor(QStringLiteral("#7f90a3"));
    const QColor fillColor(QStringLiteral("#0b1625"));
    const QColor foldColor(QStringLiteral("#142235"));
    const QColor waveColor(QStringLiteral("#9aaec2"));

    QPainterPath pagePath;
    pagePath.moveTo(14.0, 6.0);
    pagePath.lineTo(35.0, 6.0);
    pagePath.lineTo(46.0, 17.0);
    pagePath.lineTo(46.0, 50.0);
    pagePath.lineTo(14.0, 50.0);
    pagePath.closeSubpath();

    painter.setPen(QPen(borderColor, 1.8));
    painter.setBrush(fillColor);
    painter.drawPath(pagePath);

    QPainterPath foldPath;
    foldPath.moveTo(35.0, 6.0);
    foldPath.lineTo(35.0, 17.0);
    foldPath.lineTo(46.0, 17.0);
    foldPath.closeSubpath();

    painter.setBrush(foldColor);
    painter.drawPath(foldPath);

    QPainterPath wavePath;
    wavePath.moveTo(20.0, 33.0);
    wavePath.lineTo(25.0, 33.0);
    wavePath.lineTo(28.0, 25.0);
    wavePath.lineTo(32.0, 41.0);
    wavePath.lineTo(36.0, 29.0);
    wavePath.lineTo(40.0, 33.0);

    painter.setPen(QPen(waveColor, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(wavePath);

    return pixmap;
}

void setPillText(QLabel* pill, const QString& text)
{
    if (!pill) {
        return;
    }
    pill->setText(text.toHtmlEscaped());
}

int installedWaveformActionButtonWidth(const QPushButton* first,
                                       const QPushButton* second)
{
    return std::max({
        kInstalledWaveformActionButtonMinWidth,
        first ? first->sizeHint().width() : 0,
        second ? second->sizeHint().width() : 0
    });
}

int installedWaveformTypeBadgeWidth(const QLabel* label,
                                    const QString& first,
                                    const QString& second)
{
    if (!label) {
        return kInstalledWaveformTypeBadgeMinWidth;
    }

    const QFontMetrics metrics(label->font());
    const int textWidth = std::max(metrics.horizontalAdvance(first),
                                   metrics.horizontalAdvance(second));
    return std::max(kInstalledWaveformTypeBadgeMinWidth,
                    textWidth + kInstalledWaveformTypeBadgeHorizontalPadding);
}

QWidget* makeEmptyWaveformsState(QWidget* parent = nullptr)
{
    auto* state = new QWidget(parent);
    state->setAccessibleName(QObject::tr("No radio waveforms installed"));
    auto* layout = new QVBoxLayout(state);
    layout->setContentsMargins(12, 18, 12, 18);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignCenter);

    auto* icon = new QLabel(state);
    icon->setObjectName(QStringLiteral("EmptyStateIcon"));
    icon->setPixmap(makeWaveformDocumentPixmap());
    icon->setAlignment(Qt::AlignCenter);
    icon->setAccessibleName(QObject::tr("Waveform empty state icon"));
    layout->addWidget(icon, 0, Qt::AlignHCenter);
    layout->addSpacing(6);

    auto* text = new QLabel(QObject::tr("No radio waveforms installed"), state);
    text->setObjectName(QStringLiteral("EmptyStateText"));
    text->setAlignment(Qt::AlignCenter);
    text->setAccessibleName(QObject::tr("No radio waveforms installed"));
    layout->addWidget(text, 0, Qt::AlignHCenter);
    layout->addSpacing(4);

    auto* subtext = new QLabel(QObject::tr("Use Install... to add waveforms to your radio."), state);
    subtext->setObjectName(QStringLiteral("EmptyStateSubtext"));
    subtext->setAlignment(Qt::AlignCenter);
    subtext->setWordWrap(true);
    subtext->setAccessibleName(QObject::tr("Install radio waveforms hint"));
    layout->addWidget(subtext, 0, Qt::AlignHCenter);

    return state;
}

void showDockerWfpNotReadyDialog(QWidget* parent, const QString& statusText)
{
    PersistentDialog dialog(QObject::tr("Docker Waveform Unavailable"), QString(), parent);
    theme::setContainer(&dialog, QStringLiteral("dialog/waveforms/wfpNotReady"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setModal(true);
    dialog.setMinimumWidth(420);

    auto* root = new QVBoxLayout(dialog.bodyWidget());
    root->setSpacing(10);

    auto* message = new QLabel(
        QObject::tr("AetherSDR can upload a Docker waveform image only when the radio "
                    "supports the Waveform Processor and reports WFP ON and READY.\n\n"
                    "Current status: %1.\n\n"
                    "Use Install > Legacy Waveform (.ssdr_waveform)... for classic packages.")
            .arg(statusText));
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    message->setAccessibleName(QObject::tr("Docker waveform install status"));
    root->addWidget(message);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* okButton = new QPushButton(QObject::tr("OK"));
    okButton->setAccessibleName(QObject::tr("Close Docker waveform status"));
    okButton->setDefault(true);
    QObject::connect(okButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(okButton);
    root->addLayout(buttons);

    dialog.exec();
}

bool confirmRadioWaveformRemoval(QWidget* parent, const QString& name, bool isContainer)
{
    const QString title = isContainer
        ? QObject::tr("Remove Docker Container")
        : QObject::tr("Uninstall Radio Waveform");
    const QString question = isContainer
        ? QObject::tr("Remove the Docker container \"%1\" from the radio?").arg(name)
        : QObject::tr("Uninstall the waveform \"%1\" from the radio?").arg(name);
    const QString confirmText = isContainer
        ? QObject::tr("Remove")
        : QObject::tr("Uninstall");

    PersistentDialog dialog(title, QString(), parent);
    theme::setContainer(&dialog, QStringLiteral("dialog/waveforms/removeConfirm"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setModal(true);
    dialog.setMinimumWidth(380);

    auto* root = new QVBoxLayout(dialog.bodyWidget());
    root->setSpacing(10);

    auto* message = new QLabel(question);
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    message->setAccessibleName(QObject::tr("Radio waveform removal confirmation"));
    root->addWidget(message);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();

    auto* cancelButton = new QPushButton(QObject::tr("Cancel"));
    cancelButton->setAccessibleName(QObject::tr("Cancel radio waveform removal"));
    cancelButton->setDefault(true);
    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    buttons->addWidget(cancelButton);

    auto* confirmButton = new QPushButton(confirmText);
    confirmButton->setAccessibleName(confirmText);
    QObject::connect(confirmButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(confirmButton);

    root->addLayout(buttons);

    return dialog.exec() == QDialog::Accepted;
}

void showWaveformInstallResultDialog(QWidget* parent,
                                     const QString& title,
                                     const QString& messageText)
{
    PersistentDialog dialog(title, QString(), parent);
    theme::setContainer(&dialog, QStringLiteral("dialog/waveforms/installResult"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setModal(true);
    dialog.setMinimumWidth(420);

    auto* root = new QVBoxLayout(dialog.bodyWidget());
    root->setSpacing(10);

    auto* message = new QLabel(messageText);
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    message->setAccessibleName(QObject::tr("Waveform install result"));
    root->addWidget(message);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* okButton = new QPushButton(QObject::tr("OK"));
    okButton->setAccessibleName(QObject::tr("Close waveform install result"));
    okButton->setDefault(true);
    QObject::connect(okButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(okButton);
    root->addLayout(buttons);

    dialog.exec();
}

} // namespace

WaveformsDialog::WaveformsDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog(tr("Waveforms"), QStringLiteral("WaveformsDialogGeometry"), parent)
    , m_radioModel(model)
{
    theme::setContainer(this, QStringLiteral("dialog/waveforms"));
    setMinimumSize(900, 620);
    bodyWidget()->setObjectName(QStringLiteral("waveformsBody"));
    bodyWidget()->setStyleSheet(QString::fromLatin1(kWaveformsDialogStyle));

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(10);
    root->setContentsMargins(12, 10, 12, 12);

    // Radio and WFP capability/status strip.
    auto* statusFrame = makePanel(QStringLiteral("WaveformsStatusFrame"), bodyWidget());
    auto* statusRow = new QHBoxLayout(statusFrame);
    statusRow->setContentsMargins(18, 12, 18, 12);
    statusRow->setSpacing(22);

    auto* radioColumn = new QWidget(statusFrame);
    radioColumn->setObjectName(QStringLiteral("StatusColumn"));
    auto* radioLayout = new QVBoxLayout(radioColumn);
    radioLayout->setContentsMargins(0, 0, 0, 0);
    radioLayout->setSpacing(4);

    auto* radioTitle = new QLabel(tr("Connected Radio"), radioColumn);
    radioTitle->setObjectName(QStringLiteral("StatusColumnTitle"));
    radioLayout->addWidget(radioTitle);

    m_connectedRadioNameLabel = new QLabel(radioColumn);
    m_connectedRadioNameLabel->setObjectName(QStringLiteral("ConnectedRadioName"));
    m_connectedRadioNameLabel->setAccessibleName(tr("Connected radio model"));
    radioLayout->addWidget(m_connectedRadioNameLabel);

    m_connectedRadioSerialLabel = new QLabel(radioColumn);
    m_connectedRadioSerialLabel->setObjectName(QStringLiteral("ConnectedRadioSerial"));
    m_connectedRadioSerialLabel->setAccessibleName(tr("Connected radio serial number"));
    radioLayout->addWidget(m_connectedRadioSerialLabel);

    statusRow->addWidget(radioColumn, 1);
    statusRow->addWidget(makeStatusDivider(statusFrame));

    m_wfpSupportPill = makeStatusPill(tr("Radio waveform processor support"), statusFrame);
    m_wfpSupportPill->setObjectName(QStringLiteral("CapabilityPill"));
    m_wfpPowerPill = makeStatusPill(tr("Waveform processor power status"), statusFrame);
    m_wfpReadyPill = makeStatusPill(tr("Waveform processor ready status"), statusFrame);
    m_wfpIpPill = makeStatusPill(tr("Waveform processor IP address"), statusFrame);
    statusRow->addWidget(makeStatusColumn(tr("WFP Support"), m_wfpSupportPill, statusFrame), 1);
    statusRow->addWidget(makeStatusDivider(statusFrame));
    statusRow->addWidget(makeStatusColumn(tr("WFP Power"), m_wfpPowerPill, statusFrame), 1);
    statusRow->addWidget(makeStatusDivider(statusFrame));
    statusRow->addWidget(makeStatusColumn(tr("WFP Ready"), m_wfpReadyPill, statusFrame), 1);
    statusRow->addWidget(makeStatusDivider(statusFrame));
    statusRow->addWidget(makeStatusColumn(tr("WFP IP"), m_wfpIpPill, statusFrame), 1);

    root->addWidget(statusFrame);

    auto* contentRow = new QHBoxLayout;
    contentRow->setSpacing(10);
    root->addLayout(contentRow, 1);

    // Local digital-voice services live in AetherModem. The old constructor
    // block remains compile-checked temporarily, but creates no duplicate
    // controls in the Waveforms window.
    if constexpr (kShowLocalDigitalVoiceControls) {
    const DStarConfiguration initialDStarConfiguration =
        m_radioModel->dstarModel().configuration(m_radioModel->callsign());
    auto* dstarFrame = makePanel(QStringLiteral("localDigitalVoicePanel"), bodyWidget());
    dstarFrame->setMinimumWidth(410);
    auto* localRoot = new QVBoxLayout(dstarFrame);
    localRoot->setContentsMargins(18, 18, 18, 18);
    localRoot->setSpacing(14);

    auto* localHeader = new QHBoxLayout;
    localHeader->addWidget(makePanelTitle(tr("Local Digital Voice"), dstarFrame));
    localHeader->addStretch();
    localRoot->addLayout(localHeader);

    auto* localServicesScroll = new QScrollArea(dstarFrame);
    localServicesScroll->setWidgetResizable(true);
    localServicesScroll->setFrameShape(QFrame::NoFrame);
    localServicesScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    localServicesScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* localServicesScrollContent = new QWidget(localServicesScroll);
    localServicesScrollContent->setObjectName(QStringLiteral("LocalServicesScrollContent"));
    auto* localServicesScrollLayout = new QHBoxLayout(localServicesScrollContent);
    localServicesScrollLayout->setContentsMargins(0, 0, 0, 0);
    localServicesScrollLayout->setSpacing(0);

    auto* localServicesList = new QWidget(localServicesScrollContent);
    localServicesList->setObjectName(QStringLiteral("LocalServicesList"));
    auto* localServicesLayout = new QVBoxLayout(localServicesList);
    localServicesLayout->setContentsMargins(0, 0, 0, 0);
    localServicesLayout->setSpacing(14);
    localServicesScrollLayout->addWidget(localServicesList, 1);
    localServicesScrollLayout->addSpacing(kLocalServicesScrollbarGutter);

    auto* dstarServiceCard = makePanel(
        QStringLiteral("LocalDigitalVoiceServiceCard"), localServicesList);
    dstarServiceCard->setMinimumHeight(kDStarServiceCardMinHeight);
    auto* dstarOuter = new QHBoxLayout(dstarServiceCard);
    dstarOuter->setContentsMargins(0, 0, 0, 0);
    dstarOuter->setSpacing(0);

    auto* activeStrip = makePanel(QStringLiteral("ActiveServiceStrip"), dstarServiceCard);
    dstarOuter->addWidget(activeStrip);

    auto* dstarBody = new QWidget(dstarServiceCard);
    dstarBody->setObjectName(QStringLiteral("LocalServiceBody"));
    auto* dstarRoot = new QVBoxLayout(dstarBody);
    dstarRoot->setContentsMargins(16, 14, 16, 14);
    dstarRoot->setSpacing(10);
    dstarOuter->addWidget(dstarBody, 1);

    auto* dstarHeader = new QHBoxLayout;
    dstarHeader->setSpacing(12);

    dstarHeader->addWidget(makeProtocolIconLabel(tr("D-STAR"), true, dstarBody),
                           0,
                           Qt::AlignTop);

    auto* dstarTitleColumn = new QVBoxLayout;
    dstarTitleColumn->setContentsMargins(0, 0, 0, 0);
    dstarTitleColumn->setSpacing(3);
    auto* dstarTitle = makeServiceTitle(tr("Digital Voice"), dstarBody);
    dstarTitleColumn->addWidget(dstarTitle);

    auto* backendValue = new QLabel(
        tr("D-STAR - %1").arg(DigitalVoiceWaveformSettings::backendLabel(
            DigitalVoiceWaveformSettings::Backend::ThumbDv)),
        dstarBody);
    backendValue->setObjectName(QStringLiteral("ServiceSubtitle"));
    backendValue->setAccessibleName(tr("Digital voice mode and vocoder backend"));
    backendValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dstarTitleColumn->addWidget(backendValue);
    dstarHeader->addLayout(dstarTitleColumn, 1);

    m_dstarStatusLabel = new QLabel(dstarBody);
    m_dstarStatusLabel->setObjectName(QStringLiteral("digitalVoiceWaveformStatus"));
    m_dstarStatusLabel->setAccessibleName(tr("Digital voice service status"));
    m_dstarStatusLabel->setTextFormat(Qt::RichText);
    m_dstarStatusLabel->setAlignment(Qt::AlignCenter);
    m_dstarStatusLabel->setFixedHeight(kDStarStatusHeight);
    m_dstarStatusLabel->setFixedWidth(kDStarControlWidth);
    m_dstarStatusLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_dstarStartStopBtn = new QPushButton;
    m_dstarStartStopBtn->setObjectName(QStringLiteral("digitalVoiceWaveformStartStop"));
    m_dstarStartStopBtn->setAccessibleName(tr("Start or stop digital voice service"));
    m_dstarStartStopBtn->setFixedWidth(kDStarControlWidth);
    m_dstarStartStopBtn->setFixedHeight(kDStarButtonHeight);
    m_dstarStartStopBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_dstarStartStopBtn, &QPushButton::clicked,
            this, &WaveformsDialog::onDStarStartStopClicked);

    auto* dstarControlWidget = new QWidget(dstarBody);
    dstarControlWidget->setObjectName(QStringLiteral("DStarControlColumn"));
    dstarControlWidget->setFixedWidth(kDStarControlWidth);
    dstarControlWidget->setMinimumHeight(kDStarStatusHeight
                                         + kDStarControlSpacing
                                         + kDStarButtonHeight);
    dstarControlWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* dstarControlColumn = new QVBoxLayout(dstarControlWidget);
    dstarControlColumn->setContentsMargins(0, 0, 0, 0);
    dstarControlColumn->setSpacing(kDStarControlSpacing);
    dstarControlColumn->addWidget(m_dstarStatusLabel);
    dstarControlColumn->addWidget(m_dstarStartStopBtn);
    dstarHeader->addWidget(dstarControlWidget, 0, Qt::AlignTop);
    dstarRoot->addLayout(dstarHeader);

    m_dstarDetailLabel = new QLabel(dstarBody);
    m_dstarDetailLabel->setObjectName(QStringLiteral("digitalVoiceWaveformDetail"));
    m_dstarDetailLabel->setAccessibleName(tr("Digital voice service detail"));
    m_dstarDetailLabel->setTextFormat(Qt::PlainText);
    m_dstarDetailLabel->setWordWrap(true);
    m_dstarDetailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_dstarDetailLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_dstarDetailLabel->hide();
    dstarRoot->addWidget(m_dstarDetailLabel);

    m_dstarAutoStartCheck = new QCheckBox(tr("Auto-start"));
    m_dstarAutoStartCheck->setObjectName(QStringLiteral("digitalVoiceWaveformAutoStart"));
    m_dstarAutoStartCheck->setAccessibleName(tr("Auto-start digital voice service"));
    m_dstarAutoStartCheck->setChecked(initialDStarConfiguration.autoStart);
    connect(m_dstarAutoStartCheck, &QCheckBox::toggled,
            this, &WaveformsDialog::saveDStarSettings);
    dstarRoot->addWidget(m_dstarAutoStartCheck);

    dstarRoot->addWidget(makeSeparator(dstarBody));

    m_dstarSerialLabel = new QLabel(tr("ThumbDV device"));
    m_dstarSerialLabel->setObjectName(QStringLiteral("SectionLabel"));
    dstarRoot->addWidget(m_dstarSerialLabel);

    m_dstarSerialRow = new QWidget;
    m_dstarSerialRow->setObjectName(QStringLiteral("DStarSerialRow"));
    auto* serialLayout = new QHBoxLayout(m_dstarSerialRow);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    serialLayout->setSpacing(6);
    m_dstarSerialCombo = new QComboBox;
    m_dstarSerialCombo->setEditable(true);
    m_dstarSerialCombo->setInsertPolicy(QComboBox::NoInsert);
    m_dstarSerialCombo->setObjectName(QStringLiteral("dstarThumbDvSerialPort"));
    m_dstarSerialCombo->setAccessibleName(tr("ThumbDV serial port"));
    m_dstarSerialCombo->setMinimumWidth(0);
    m_dstarSerialCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_dstarSerialCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    applyComboHeight(m_dstarSerialCombo);
    m_dstarSerialLabel->setBuddy(m_dstarSerialCombo);
#if defined(Q_OS_WIN)
    m_dstarSerialCombo->lineEdit()->setPlaceholderText(tr("COM3"));
#elif defined(Q_OS_MAC)
    m_dstarSerialCombo->lineEdit()->setPlaceholderText(tr("/dev/cu.usbserial-*"));
#else
    m_dstarSerialCombo->lineEdit()->setPlaceholderText(tr("/dev/ttyUSB0"));
#endif
    populateDStarSerialPorts(initialDStarConfiguration.serialPort);
    connect(m_dstarSerialCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &WaveformsDialog::saveDStarSettings);
    connect(m_dstarSerialCombo->lineEdit(), &QLineEdit::editingFinished,
            this, &WaveformsDialog::saveDStarSettings);
    serialLayout->addWidget(m_dstarSerialCombo, 1);
    m_dstarSerialMenuBtn = new QToolButton;
    m_dstarSerialMenuBtn->setObjectName(QStringLiteral("ThumbDvDeviceMenu"));
    m_dstarSerialMenuBtn->setArrowType(Qt::DownArrow);
    m_dstarSerialMenuBtn->setAccessibleName(tr("Open ThumbDV device choices"));
    m_dstarSerialMenuBtn->setToolTip(tr("Open ThumbDV device choices"));
    m_dstarSerialMenuBtn->setMinimumHeight(m_dstarSerialCombo->minimumHeight());
    m_dstarSerialMenuBtn->setFixedWidth(m_dstarSerialCombo->minimumHeight());
    connect(m_dstarSerialMenuBtn, &QToolButton::clicked,
            m_dstarSerialCombo, qOverload<>(&QComboBox::showPopup));
    serialLayout->addWidget(m_dstarSerialMenuBtn);
    m_dstarSerialRefreshBtn = new QPushButton(tr("Refresh"));
    m_dstarSerialRefreshBtn->setAccessibleName(tr("Refresh ThumbDV serial ports"));
    m_dstarSerialRefreshBtn->setMinimumHeight(m_dstarSerialCombo->minimumHeight());
    connect(m_dstarSerialRefreshBtn, &QPushButton::clicked, this, [this] {
        if (m_radioModel) {
            m_radioModel->dstarModel().refreshSerialDevices();
        }
    });
    serialLayout->addWidget(m_dstarSerialRefreshBtn);
    dstarRoot->addWidget(m_dstarSerialRow);

    m_dstarAdvancedBtn = new QToolButton;
    m_dstarAdvancedBtn->setObjectName(QStringLiteral("DStarAdvancedButton"));
    m_dstarAdvancedBtn->setText(tr("Advanced"));
    m_dstarAdvancedBtn->setAccessibleName(tr("Show advanced D-STAR waveform settings"));
    m_dstarAdvancedBtn->setCheckable(true);
    m_dstarAdvancedBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_dstarAdvancedBtn->setChecked(
        !initialDStarConfiguration.executablePath.trimmed().isEmpty());
    m_dstarAdvancedBtn->setArrowType(
        m_dstarAdvancedBtn->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    dstarRoot->addWidget(m_dstarAdvancedBtn);

    m_dstarAdvancedPanel = new QWidget;
    m_dstarAdvancedPanel->setObjectName(QStringLiteral("DStarAdvancedPanel"));
    auto* advancedLayout = new QVBoxLayout(m_dstarAdvancedPanel);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->setSpacing(4);

    auto* routingLabel = new QLabel(tr("Station and routing"));
    routingLabel->setObjectName(QStringLiteral("SectionLabel"));
    advancedLayout->addWidget(routingLabel);

    auto* routingGrid = new QGridLayout;
    routingGrid->setContentsMargins(0, 0, 0, 8);
    routingGrid->setHorizontalSpacing(8);
    routingGrid->setVerticalSpacing(6);

    auto makeRoutingEdit = [this](const QString& value,
                                  int maximumLength,
                                  const QString& accessibleName,
                                  bool allowSpaces,
                                  bool allowLeadingSlash = false) {
        auto* edit = new QLineEdit(value);
        edit->setMaxLength(maximumLength);
        edit->setMinimumHeight(kWaveformInputMinHeight);
        edit->setAccessibleName(accessibleName);
        const QString characters = allowSpaces
            ? QStringLiteral("A-Za-z0-9 ")
            : QStringLiteral("A-Za-z0-9");
        const QString pattern = allowLeadingSlash
            ? QStringLiteral("^/?[%1]{0,%2}$").arg(characters).arg(maximumLength)
            : QStringLiteral("^[%1]{0,%2}$").arg(characters).arg(maximumLength);
        edit->setValidator(new QRegularExpressionValidator(
            QRegularExpression(pattern),
            edit));
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, maximumLength] {
            const QString normalized =
                DigitalVoiceWaveformSettings::normalizeCallsign(edit->text(), maximumLength);
            if (edit->text() != normalized) {
                const QSignalBlocker blocker(edit);
                edit->setText(normalized);
            }
            saveDStarSettings();
        });
        return edit;
    };

    const QString initialMyCall = initialDStarConfiguration.myCall;
    m_dstarMyCallEdit = makeRoutingEdit(initialMyCall, 8, tr("D-STAR MYCALL"), false);
    m_dstarMyCallEdit->setObjectName(QStringLiteral("dstarMyCall"));
    m_dstarMyCallSuffixEdit = makeRoutingEdit(
        initialDStarConfiguration.myCallSuffix, 4, tr("D-STAR MYCALL suffix"), false);
    m_dstarMyCallSuffixEdit->setObjectName(QStringLiteral("dstarMyCallSuffix"));
    m_dstarUrCallEdit = makeRoutingEdit(
        initialDStarConfiguration.urCall, 8, tr("D-STAR URCALL"), true, true);
    m_dstarUrCallEdit->setObjectName(QStringLiteral("dstarUrCall"));
    m_dstarRpt1Edit = makeRoutingEdit(
        initialDStarConfiguration.rpt1, 8, tr("D-STAR RPT1"), true);
    m_dstarRpt1Edit->setObjectName(QStringLiteral("dstarRpt1"));
    m_dstarRpt2Edit = makeRoutingEdit(
        initialDStarConfiguration.rpt2, 8, tr("D-STAR RPT2"), true);
    m_dstarRpt2Edit->setObjectName(QStringLiteral("dstarRpt2"));
    m_dstarMessageEdit = new QLineEdit(initialDStarConfiguration.message);
    m_dstarMessageEdit->setObjectName(QStringLiteral("dstarMessage"));
    m_dstarMessageEdit->setMaxLength(20);
    m_dstarMessageEdit->setMinimumHeight(kWaveformInputMinHeight);
    m_dstarMessageEdit->setAccessibleName(tr("D-STAR message"));
    m_dstarMessageEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^[\\x20-\\x7B\\x7D-\\x7E]{0,20}$")),
        m_dstarMessageEdit));
    connect(m_dstarMessageEdit, &QLineEdit::editingFinished,
            this, &WaveformsDialog::saveDStarSettings);

    auto addRoutingRow = [routingGrid](int row,
                                      const QString& text,
                                      QLineEdit* edit,
                                      int columnSpan = 3) {
        auto* label = new QLabel(text);
        label->setBuddy(edit);
        routingGrid->addWidget(label, row, 0);
        routingGrid->addWidget(edit, row, 1, 1, columnSpan);
    };
    auto* myCallLabel = new QLabel(tr("MYCALL"));
    myCallLabel->setBuddy(m_dstarMyCallEdit);
    routingGrid->addWidget(myCallLabel, 0, 0);
    routingGrid->addWidget(m_dstarMyCallEdit, 0, 1);
    auto* suffixLabel = new QLabel(tr("Suffix"));
    suffixLabel->setBuddy(m_dstarMyCallSuffixEdit);
    routingGrid->addWidget(suffixLabel, 0, 2);
    routingGrid->addWidget(m_dstarMyCallSuffixEdit, 0, 3);
    addRoutingRow(1, tr("URCALL"), m_dstarUrCallEdit);
    addRoutingRow(2, tr("RPT1"), m_dstarRpt1Edit);
    addRoutingRow(3, tr("RPT2"), m_dstarRpt2Edit);
    addRoutingRow(4, tr("Message"), m_dstarMessageEdit);
    routingGrid->setColumnStretch(1, 1);
    routingGrid->setColumnStretch(3, 1);
    advancedLayout->addLayout(routingGrid);

    auto* exeRow = new QWidget;
    exeRow->setObjectName(QStringLiteral("DStarExecutableRow"));
    auto* exeLayout = new QHBoxLayout(exeRow);
    exeLayout->setContentsMargins(0, 0, 0, 0);
    exeLayout->setSpacing(6);
    m_dstarExecutableEdit = new QLineEdit(initialDStarConfiguration.executablePath);
    m_dstarExecutableEdit->setObjectName(QStringLiteral("digitalVoiceWaveformExecutable"));
    m_dstarExecutableEdit->setAccessibleName(tr("Digital voice service executable"));
    m_dstarExecutableEdit->setMinimumHeight(kWaveformInputMinHeight);
    m_dstarExecutableEdit->setMinimumWidth(0);
    m_dstarExecutableEdit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    const QString defaultExecutablePath = DigitalVoiceWaveformProcess::defaultExecutablePath();
    const QString defaultExecutableName = QFileInfo(defaultExecutablePath).fileName();
    m_dstarExecutableEdit->setPlaceholderText(
        tr("Default: %1").arg(defaultExecutableName.isEmpty()
            ? defaultExecutablePath
            : defaultExecutableName));
    auto updateExecutableHint = [this, defaultExecutablePath](const QString& path) {
        const QString trimmed = path.trimmed();
        const QString hint = trimmed.isEmpty()
            ? tr("Leave blank to use the default executable: %1").arg(defaultExecutablePath)
            : trimmed;
        m_dstarExecutableEdit->setToolTip(hint);
        m_dstarExecutableEdit->setAccessibleDescription(hint);
    };
    updateExecutableHint(m_dstarExecutableEdit->text());
    m_dstarExecutableEdit->setCursorPosition(m_dstarExecutableEdit->text().size());
    connect(m_dstarExecutableEdit, &QLineEdit::textChanged,
            this, updateExecutableHint);
    connect(m_dstarExecutableEdit, &QLineEdit::editingFinished,
            this, &WaveformsDialog::saveDStarSettings);
    exeLayout->addWidget(m_dstarExecutableEdit, 1);
    m_dstarBrowseBtn = new QPushButton(tr("Browse..."));
    m_dstarBrowseBtn->setAccessibleName(tr("Browse for digital voice service executable"));
    m_dstarBrowseBtn->setMinimumHeight(kWaveformInputMinHeight);
    connect(m_dstarBrowseBtn, &QPushButton::clicked,
            this, &WaveformsDialog::onDStarBrowseClicked);
    exeLayout->addWidget(m_dstarBrowseBtn);
    exeRow->setMinimumHeight(kWaveformInputMinHeight);
    auto* executableLabel = new QLabel(tr("Executable"));
    executableLabel->setBuddy(m_dstarExecutableEdit);
    advancedLayout->addWidget(executableLabel);
    advancedLayout->addWidget(exeRow);
    m_dstarAdvancedPanel->setVisible(m_dstarAdvancedBtn->isChecked());
    auto updateDStarCardMinimumHeight = [dstarServiceCard, dstarBody]() {
        dstarServiceCard->setMinimumHeight(qMax(kDStarServiceCardMinHeight,
                                                dstarBody->sizeHint().height()));
        dstarServiceCard->updateGeometry();
    };
    updateDStarCardMinimumHeight();
    connect(m_dstarAdvancedBtn, &QToolButton::toggled, this, [this](bool checked) {
        m_dstarAdvancedBtn->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        m_dstarAdvancedPanel->setVisible(checked);
        if (checked) {
            m_dstarExecutableEdit->setCursorPosition(m_dstarExecutableEdit->text().size());
        }
    });
    connect(m_dstarAdvancedBtn, &QToolButton::toggled, this, updateDStarCardMinimumHeight);
    dstarRoot->addWidget(m_dstarAdvancedPanel);
    dstarServiceCard->setVisible(kLocalDigitalVoiceWaveformAvailable);
    localServicesLayout->addWidget(dstarServiceCard);
    localServicesLayout->addStretch();
    localServicesScroll->setWidget(localServicesScrollContent);
    localRoot->addWidget(localServicesScroll, 1);

    contentRow->addWidget(dstarFrame, 1);
    }

    // Radio waveform processor / installed radio waveforms.
    auto* installedFrame = makePanel(QStringLiteral("installedWaveformsPanel"), bodyWidget());
    auto* installedRoot = new QVBoxLayout(installedFrame);
    installedRoot->setContentsMargins(18, 18, 18, 18);
    installedRoot->setSpacing(14);

    auto* processorHeader = new QHBoxLayout;
    auto* processorTitle = makePanelTitle(tr("Radio Waveform Processor"), installedFrame);
    processorHeader->addWidget(processorTitle);
    processorHeader->addStretch();
    installedRoot->addLayout(processorHeader);

    auto* processorDescription = makeMutedLabel(
        tr("Legacy waveform packages (.ssdr_waveform) can be installed on compatible radios. "
           "Some newer radios also include a Waveform Processor for Docker waveform images; "
           "Docker installs require WFP support, power, and ready state."),
        installedFrame);
    processorDescription->setWordWrap(true);
    processorDescription->setAccessibleName(tr("Radio waveform processor description"));
    installedRoot->addWidget(processorDescription);

    installedRoot->addWidget(makeSeparator(installedFrame));

    auto* installedHeader = new QHBoxLayout;
    auto* installedTitle = makePanelTitle(tr("Installed Waveforms"), installedFrame);
    installedHeader->addWidget(installedTitle);
    installedHeader->addStretch();

    m_installBtn = new QToolButton;
    m_installBtn->setText(tr("Install..."));
    m_installBtn->setAccessibleName(tr("Install radio waveform"));
    m_installBtn->setPopupMode(QToolButton::InstantPopup);
    m_installBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_installBtn->setFixedWidth(104);
    m_installBtn->setEnabled(false);  // updated after installer state is known
    auto* installMenu = new QMenu(m_installBtn);
    installMenu->setStyleSheet(QString::fromLatin1(kWaveformsInstallMenuStyle));
    installMenu->addAction(tr("Legacy Waveform (.ssdr_waveform)..."),
                           this, &WaveformsDialog::onInstallLegacyClicked);
    m_installDockerAction = installMenu->addAction(tr("Docker Waveform Image..."),
                                                   this, &WaveformsDialog::onInstallDockerClicked);
    m_installBtn->setMenu(installMenu);
    installedHeader->addWidget(m_installBtn);
    installedRoot->addLayout(installedHeader);

    auto* listFrame = makePanel(QStringLiteral("WaveformListFrame"), installedFrame);
    listFrame->setMinimumHeight(170);
    auto* listFrameLayout = new QVBoxLayout(listFrame);
    listFrameLayout->setContentsMargins(0, 0, 0, 0);
    listFrameLayout->setSpacing(0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    m_listContainer = new QWidget;
    m_listLayout = new QVBoxLayout(m_listContainer);
    m_listLayout->setSpacing(4);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->addStretch();

    scroll->setWidget(m_listContainer);
    listFrameLayout->addWidget(scroll);
    installedRoot->addWidget(listFrame, 1);
    contentRow->addWidget(installedFrame, 1);

    // ── Wire model + theme signals ────────────────────────────────────────────
    FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    connect(&wfModel, &FlexWaveformModel::wfpStatusChanged,
            this, &WaveformsDialog::refreshStatus);
    connect(&wfModel, &FlexWaveformModel::wfpStatusChanged,
            this, &WaveformsDialog::updateInstallButtonState);
    connect(&wfModel, &FlexWaveformModel::waveformsChanged,
            this, &WaveformsDialog::refreshWaveformList);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &WaveformsDialog::refreshStatus);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &WaveformsDialog::refreshWaveformList);
    if constexpr (kShowLocalDigitalVoiceControls) {
        connect(&m_radioModel->dstarModel(), &DStarModel::serialDevicesChanged,
                this, [this] {
            const DStarConfiguration config =
                m_radioModel->dstarModel().configuration(m_radioModel->callsign());
            populateDStarSerialPorts(config.serialPort);
        });
        connect(&m_radioModel->dstarModel(), &DStarModel::configurationChanged,
                this, &WaveformsDialog::refreshDStarConfiguration);
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, &WaveformsDialog::refreshDStarStatus);
        connect(&DigitalVoiceWaveformProcess::instance(), &DigitalVoiceWaveformProcess::stateChanged,
                this, &WaveformsDialog::refreshDStarStatus);
        connect(&DigitalVoiceWaveformProcess::instance(), &DigitalVoiceWaveformProcess::statusTextChanged,
                this, &WaveformsDialog::refreshDStarStatus);
        connect(&DigitalVoiceWaveformProcess::instance(), &DigitalVoiceWaveformProcess::metricsChanged,
                this, &WaveformsDialog::refreshDStarStatus);
        connect(&DigitalVoiceWaveformProcess::instance(), &DigitalVoiceWaveformProcess::healthChanged,
                this, &WaveformsDialog::refreshDStarStatus);
    }
    connect(m_radioModel, &RadioModel::connectionStateChanged,
            this, &WaveformsDialog::refreshStatus);
    if constexpr (kShowLocalDigitalVoiceControls) {
        connect(m_radioModel, &RadioModel::connectionStateChanged,
                this, &WaveformsDialog::updateDStarControls);
    }
    connect(m_radioModel, &RadioModel::connectionStateChanged,
            this, &WaveformsDialog::updateInstallButtonState);
    connect(m_radioModel, &RadioModel::infoChanged,
            this, &WaveformsDialog::refreshStatus);
    connect(m_radioModel, &RadioModel::infoChanged,
            this, &WaveformsDialog::updateInstallButtonState);
    // The WFP support pill and the Docker-install gate no longer read the "wfp"
    // license feature (#4210) — both follow WFP hardware/runtime state — so the
    // former licenseFeaturesChanged refresh connections are no longer needed.

    refreshStatus();
    updateInstallButtonState();
    refreshWaveformList();
    if constexpr (kShowLocalDigitalVoiceControls) {
        refreshDStarConfiguration();
        refreshDStarStatus();
    }
}

void WaveformsDialog::updateInstallButtonState()
{
    const bool busy  = m_installer && m_installer->isInstalling();
    const bool connected = m_radioModel && m_radioModel->isConnected();
    m_installBtn->setEnabled(!busy && connected);

    if (!m_installDockerAction || !m_radioModel) {
        return;
    }

    const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    const QString dockerBlocker = connected
        ? dockerInstallBlockerText(m_radioModel, wfModel)
        : tr("Connect to a radio before installing Docker waveform images.");
    const bool dockerReady = connected && dockerBlocker.isEmpty();
    m_installDockerAction->setEnabled(!busy && dockerReady);
    if (busy) {
        m_installDockerAction->setToolTip(tr("A waveform install is already in progress."));
    } else if (dockerReady) {
        m_installDockerAction->setToolTip(tr("Install a Docker waveform image on the radio."));
    } else {
        m_installDockerAction->setToolTip(dockerBlocker);
    }
    m_installBtn->setToolTip(connected
        ? tr("Install or manage waveforms for the connected radio")
        : tr("Connect to a radio before installing radio waveforms"));
}

void WaveformsDialog::onInstallLegacyClicked()
{
    installWaveformFile(
        tr("Select Legacy Waveform Package (.ssdr_waveform)"),
        tr("SmartSDR Waveforms (*.ssdr_waveform);;All Files (*)"),
        false,
        legacyWaveformDialogInitialPath());
}

void WaveformsDialog::onInstallDockerClicked()
{
    const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    const QString blocker = dockerInstallBlockerText(m_radioModel, wfModel);
    if (!blocker.isEmpty()) {
        showDockerWfpNotReadyDialog(this, blocker);
        return;
    }

    installWaveformFile(
        tr("Select Docker Waveform Image"),
        tr("Docker Waveform Images (*.tar *.tar.gz *.tgz);;All Files (*)"),
        true);
}

void WaveformsDialog::installWaveformFile(const QString& title,
                                          const QString& filter,
                                          bool docker,
                                          const QString& initialPath)
{
    if (!m_radioModel || !m_radioModel->isConnected()) {
        return;
    }

    const QString path = QFileDialog::getOpenFileName(
        this,
        title,
        initialPath,
        filter);

    if (path.isEmpty()) {
        return;
    }

    if (docker) {
        const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
        const QString blocker = dockerInstallBlockerText(m_radioModel, wfModel);
        if (!blocker.isEmpty()) {
            showDockerWfpNotReadyDialog(this, blocker);
            return;
        }
    }

    if (!m_installer) {
        m_installer = new WaveformInstaller(m_radioModel, this);
    }

    if (m_installer->isInstalling()) {
        return;
    }

    m_installBtn->setEnabled(false);

    auto* progress = new PersistentDialog(
        docker ? tr("Installing Docker Waveform")
               : tr("Installing Legacy Waveform"),
        QString(),
        this);
    theme::setContainer(progress, QStringLiteral("dialog/waveforms/installProgress"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setModal(true);
    progress->setMinimumWidth(420);

    auto* progressRoot = new QVBoxLayout(progress->bodyWidget());
    progressRoot->setSpacing(10);

    auto* progressLabel = new QLabel(
        docker ? tr("Installing Docker waveform...")
               : tr("Installing legacy waveform..."));
    progressLabel->setTextFormat(Qt::PlainText);
    progressLabel->setWordWrap(true);
    progressLabel->setAccessibleName(tr("Waveform install progress"));
    progressRoot->addWidget(progressLabel);

    auto* progressBar = new QProgressBar;
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setTextVisible(true);
    progressBar->setAccessibleName(tr("Waveform install progress value"));
    progressRoot->addWidget(progressBar);

    auto* progressButtons = new QHBoxLayout;
    progressButtons->addStretch();
    auto* cancelButton = new QPushButton(tr("Cancel"));
    cancelButton->setAccessibleName(tr("Cancel waveform install"));
    progressButtons->addWidget(cancelButton);
    progressRoot->addLayout(progressButtons);

    connect(m_installer, &WaveformInstaller::progressChanged,
            progress, [progressBar, progressLabel](int pct, const QString& msg) {
                progressBar->setValue(pct);
                progressLabel->setText(msg);
            });

    connect(cancelButton, &QPushButton::clicked,
            progress, &QDialog::reject);
    connect(progress, &QDialog::rejected, this, [this] {
        if (m_installer && m_installer->isInstalling()) {
            m_installer->cancel();
        }
    });

    connect(m_installer, &WaveformInstaller::finished,
            this, [this, progress](bool ok, const QString& msg) {
                progress->close();
                progress->deleteLater();
                updateInstallButtonState();
                showWaveformInstallResultDialog(
                    this,
                    ok ? tr("Install Complete") : tr("Install Failed"),
                    msg);
            }, Qt::SingleShotConnection);

    progress->show();

    if (docker) {
        m_installer->installDockerWaveform(path);
    } else {
        m_installer->installLegacyWaveform(path);
    }
}

void WaveformsDialog::onDStarStartStopClicked()
{
    if (!kLocalDigitalVoiceWaveformAvailable) {
        return;
    }

    if (!saveDStarConfiguration()) {
        return;
    }

    DigitalVoiceWaveformProcess& process = DigitalVoiceWaveformProcess::instance();
    if (process.isActive()) {
        process.stop();
    } else {
        if (!process.startForRadio(m_radioModel->radioAddress(), m_radioModel->callsign())
                && process.lastError().contains(QStringLiteral("MYCALL"))) {
            m_dstarAdvancedBtn->setChecked(true);
            m_dstarMyCallEdit->setFocus();
            m_dstarMyCallEdit->selectAll();
        }
    }
    refreshDStarStatus();
}

void WaveformsDialog::onDStarBrowseClicked()
{
    const QString initialPath =
        dstarExecutableBrowseStartPath(m_dstarExecutableEdit->text().trimmed());
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select Digital Voice Service Executable"),
        initialPath);
    if (path.isEmpty()) {
        return;
    }
    m_dstarExecutableEdit->setText(path);
    m_dstarExecutableEdit->setCursorPosition(path.size());
    m_dstarExecutableEdit->setFocus();
    saveDStarSettings();
}

void WaveformsDialog::refreshStatus()
{
    auto setStatusPills = [this](const QString& support,
                                 const QString& power,
                                 const QString& ready,
                                 const QString& ip) {
        setPillText(m_wfpSupportPill, support);
        setPillText(m_wfpPowerPill, power);
        setPillText(m_wfpReadyPill, ready);
        setPillText(m_wfpIpPill, ip);
    };

    auto setSupportHint = [this](const QString& hint) {
        if (m_wfpSupportPill) {
            m_wfpSupportPill->setToolTip(hint);
            m_wfpSupportPill->setAccessibleDescription(hint);
        }
    };

    if (!m_radioModel || !m_radioModel->isConnected()) {
        if (m_connectedRadioNameLabel) {
            m_connectedRadioNameLabel->setText(tr("No radio"));
        }
        if (m_connectedRadioSerialLabel) {
            m_connectedRadioSerialLabel->setText(tr("SN: none"));
        }
        setStatusPills(tr("No radio"), tr("OFF"), tr("NOT READY"), tr("IP: none"));
        const QString hint = tr("Connect to a radio to query Waveform Processor support.");
        setSupportHint(hint);
        updateInstallButtonState();
        return;
    }

    QString radioName = m_radioModel->model().trimmed();
    if (radioName.isEmpty()) {
        radioName = m_radioModel->name().trimmed();
    }
    if (radioName.isEmpty()) {
        radioName = tr("Connected radio");
    }
    const QString radioNickname = m_radioModel->nickname().trimmed();
    if (!radioNickname.isEmpty() && radioNickname != radioName) {
        radioName = tr("%1  %2").arg(radioName, radioNickname);
    }
    QString serial = m_radioModel->serial().trimmed();
    if (serial.isEmpty()) {
        serial = m_radioModel->chassisSerial().trimmed();
    }
    if (serial.isEmpty()) {
        serial = tr("unavailable");
    }
    if (m_connectedRadioNameLabel) {
        m_connectedRadioNameLabel->setText(radioName);
    }
    if (m_connectedRadioSerialLabel) {
        m_connectedRadioSerialLabel->setText(tr("SN: %1").arg(serial));
    }

    const WfpSupportUiState support = wfpSupportUiState(m_radioModel);
    const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();

    const QString powerText  = wfModel.wfpPowered() ? tr("ON")    : tr("OFF");
    const QString readyText  = wfModel.wfpReady()   ? tr("READY") : tr("NOT READY");

    QString ip = wfModel.wfpIpAddress();
    if (ip.isEmpty()) {
        ip = tr("none");
    }

    setStatusPills(support.label, powerText, readyText, tr("IP: %1").arg(ip));
    setSupportHint(support.hint);
    updateInstallButtonState();
}

void WaveformsDialog::refreshDStarStatus()
{
    const DigitalVoiceWaveformProcess& process = DigitalVoiceWaveformProcess::instance();
    const QString stateText = DigitalVoiceWaveformProcess::stateName(process.state());
    const bool degraded = isDegradedDigitalVoiceWaveformHealth(process.health());
    const QString processDetail = process.statusText().trimmed();
    const QString detailText = degraded
        ? process.healthDetail()
        : processDetail;

    setPillText(m_dstarStatusLabel, stateText);
    if (m_dstarStatusLabel) {
        const QString healthText = DigitalVoiceWaveformProcess::healthName(process.health());
        m_dstarStatusLabel->setToolTip(
            detailText.isEmpty()
                ? tr("%1 - %2").arg(stateText, healthText)
                : detailText);
    }
    if (m_dstarDetailLabel) {
        const bool showDetail = !detailText.isEmpty() && detailText != stateText;
        m_dstarDetailLabel->setText(detailText);
        m_dstarDetailLabel->setToolTip(detailText);
        m_dstarDetailLabel->setVisible(showDetail);
        ThemeManager::instance().applyStyleSheet(
            m_dstarDetailLabel,
            degraded
                ? "QLabel { color: {{color.accent.warning}}; }"
                : "QLabel { color: {{color.text.secondary}}; }");
    }

    updateDStarControls();
}

void WaveformsDialog::updateDStarControls()
{
    const DigitalVoiceWaveformProcess& process = DigitalVoiceWaveformProcess::instance();
    const bool active = process.isActive();
    const bool stopping = process.state() == DigitalVoiceWaveformProcess::State::Stopping;
    m_dstarStartStopBtn->setText(active ? tr("Stop") : tr("Start"));
    m_dstarStartStopBtn->setEnabled(kLocalDigitalVoiceWaveformAvailable
                                    && !stopping
                                    && (active || m_radioModel->isConnected()));
    m_dstarExecutableEdit->setEnabled(true);
    m_dstarExecutableEdit->setReadOnly(active || stopping);
    m_dstarBrowseBtn->setEnabled(!active && !stopping);
    const QList<QLineEdit*> routingEdits {
        m_dstarMyCallEdit,
        m_dstarMyCallSuffixEdit,
        m_dstarUrCallEdit,
        m_dstarRpt1Edit,
        m_dstarRpt2Edit,
        m_dstarMessageEdit
    };
    for (QLineEdit* edit : routingEdits) {
        edit->setReadOnly(active || stopping);
    }
    m_dstarSerialLabel->setVisible(true);
    m_dstarSerialRow->setVisible(true);
    m_dstarSerialCombo->setEnabled(!active && !stopping);
    m_dstarSerialMenuBtn->setEnabled(!active && !stopping);
    m_dstarSerialRefreshBtn->setEnabled(!active && !stopping);
}

void WaveformsDialog::saveDStarSettings()
{
    saveDStarConfiguration();
}

bool WaveformsDialog::saveDStarConfiguration()
{
    if (m_updatingDStarConfiguration || !m_radioModel) {
        return false;
    }

    DStarConfiguration config =
        m_radioModel->dstarModel().configuration(m_radioModel->callsign());
    config.autoStart = m_dstarAutoStartCheck->isChecked();
    config.executablePath = m_dstarExecutableEdit->text();
    config.serialPort = selectedDStarSerialPort();
    config.myCall = m_dstarMyCallEdit->text();
    config.myCallSuffix = m_dstarMyCallSuffixEdit->text();
    config.urCall = m_dstarUrCallEdit->text();
    config.rpt1 = m_dstarRpt1Edit->text();
    config.rpt2 = m_dstarRpt2Edit->text();
    config.message = m_dstarMessageEdit->text();

    QString error;
    if (!m_radioModel->dstarModel().setConfiguration(
            config, m_radioModel->callsign(), &error)) {
        refreshDStarConfiguration();
        if (m_dstarDetailLabel) {
            m_dstarDetailLabel->setText(error);
            m_dstarDetailLabel->setToolTip(error);
            m_dstarDetailLabel->setVisible(true);
        }
        return false;
    }
    updateDStarControls();
    return true;
}

void WaveformsDialog::refreshDStarConfiguration()
{
    if (!m_radioModel || !m_dstarAutoStartCheck) {
        return;
    }

    m_updatingDStarConfiguration = true;
    const DStarConfiguration config =
        m_radioModel->dstarModel().configuration(m_radioModel->callsign());
    const QSignalBlocker autoStartBlocker(m_dstarAutoStartCheck);
    const QSignalBlocker executableBlocker(m_dstarExecutableEdit);
    const QSignalBlocker myCallBlocker(m_dstarMyCallEdit);
    const QSignalBlocker suffixBlocker(m_dstarMyCallSuffixEdit);
    const QSignalBlocker urCallBlocker(m_dstarUrCallEdit);
    const QSignalBlocker rpt1Blocker(m_dstarRpt1Edit);
    const QSignalBlocker rpt2Blocker(m_dstarRpt2Edit);
    const QSignalBlocker messageBlocker(m_dstarMessageEdit);
    m_dstarAutoStartCheck->setChecked(config.autoStart);
    m_dstarExecutableEdit->setText(config.executablePath);
    m_dstarMyCallEdit->setText(config.myCall);
    m_dstarMyCallSuffixEdit->setText(config.myCallSuffix);
    m_dstarUrCallEdit->setText(config.urCall);
    m_dstarRpt1Edit->setText(config.rpt1);
    m_dstarRpt2Edit->setText(config.rpt2);
    m_dstarMessageEdit->setText(config.message);
    populateDStarSerialPorts(config.serialPort);
    m_updatingDStarConfiguration = false;
}

void WaveformsDialog::populateDStarSerialPorts(const QString& preferredPort)
{
    const QString preferred = preferredPort.trimmed();
    const QSignalBlocker comboBlocker(m_dstarSerialCombo);
    const QSignalBlocker editBlocker(m_dstarSerialCombo->lineEdit());

    m_dstarSerialCombo->clear();
    const QList<DStarSerialDevice> options = m_radioModel
        ? m_radioModel->dstarModel().serialDevices()
        : QList<DStarSerialDevice>{};

    int selectedIndex = -1;
    if (preferred.isEmpty()) {
        m_dstarSerialCombo->addItem(tr("Select a ThumbDV device"), QString());
        selectedIndex = 0;
    }
    for (const DStarSerialDevice& option : options) {
        QString label = option.label;
        if (option.verification == DStarSerialDevice::Verification::Probing) {
            label += tr(" (checking)");
        } else if (option.verification == DStarSerialDevice::Verification::Unavailable) {
            label += option.present ? tr(" (not verified)") : tr(" (not connected)");
        }
        m_dstarSerialCombo->addItem(label, option.path);
        const int index = m_dstarSerialCombo->count() - 1;
        m_dstarSerialCombo->setItemData(
            index,
            option.detail.isEmpty()
                ? option.path
                : QStringLiteral("%1\n%2").arg(option.path, option.detail),
            Qt::ToolTipRole);
        if (!preferred.isEmpty()
            && DStarModel::serialPathsEquivalent(option.path, preferred)) {
            selectedIndex = index;
        }
    }

    if (!preferred.isEmpty() && selectedIndex < 0) {
        m_dstarSerialCombo->insertItem(0, preferred, preferred);
        selectedIndex = 0;
    }

    if (selectedIndex >= 0) {
        m_dstarSerialCombo->setCurrentIndex(selectedIndex);
    } else {
        m_dstarSerialCombo->lineEdit()->clear();
    }
}

QString WaveformsDialog::selectedDStarSerialPort() const
{
    if (!m_dstarSerialCombo) {
        return {};
    }

    const int index = m_dstarSerialCombo->currentIndex();
    const QString editText = m_dstarSerialCombo->currentText().trimmed();
    if (index >= 0 && editText == m_dstarSerialCombo->itemText(index)) {
        const QString path = m_dstarSerialCombo->itemData(index).toString().trimmed();
        if (!path.isEmpty()) {
            return path;
        }
    }
    return editText;
}

void WaveformsDialog::refreshWaveformList()
{
    // Remove all items except the trailing stretch
    while (m_listLayout->count() > 1) {
        QLayoutItem* item = m_listLayout->takeAt(0);
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }

    const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    const QList<FlexWaveformEntry>& waveforms = wfModel.waveforms();

    if (waveforms.isEmpty()) {
        m_listLayout->insertWidget(0, makeEmptyWaveformsState(m_listContainer));
        return;
    }

    for (const FlexWaveformEntry& entry : waveforms) {
        const QString name        = entry.name;
        const bool    isContainer = entry.isContainer;

        auto* row = new QFrame;
        row->setObjectName(QStringLiteral("RadioWaveformRow"));
        row->setFrameShape(QFrame::StyledPanel);
        row->setAttribute(Qt::WA_StyledBackground, true);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 7, 10, 7);
        rowLayout->setSpacing(8);

        // Name + version
        auto* nameLabel = new QLabel(
            QStringLiteral("<b>%1</b> %2")
                .arg(name.toHtmlEscaped(), entry.version.toHtmlEscaped()));
        nameLabel->setTextFormat(Qt::RichText);
        nameLabel->setAccessibleName(tr("Radio waveform %1").arg(name));
        nameLabel->setMinimumWidth(0);
        nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        nameLabel->setToolTip(QStringLiteral("%1 %2").arg(name, entry.version).trimmed());
        rowLayout->addWidget(nameLabel, 1);

        // Type badge
        const QString dockerBadgeText = tr("Docker");
        const QString legacyBadgeText = tr("Legacy");
        const QString badgeText = isContainer ? dockerBadgeText : legacyBadgeText;
        auto* typeLabel = new QLabel(badgeText);
        typeLabel->setObjectName(QStringLiteral("WaveformTypeBadge"));
        typeLabel->setAccessibleName(tr("Radio waveform type %1").arg(badgeText));
        typeLabel->setAlignment(Qt::AlignCenter);
        typeLabel->setFixedWidth(installedWaveformTypeBadgeWidth(typeLabel,
                                                                 dockerBadgeText,
                                                                 legacyBadgeText));
        typeLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        rowLayout->addWidget(typeLabel);

        auto* actionWidget = new QWidget(row);
        actionWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        auto* actionLayout = new QHBoxLayout(actionWidget);
        actionLayout->setContentsMargins(0, 0, 0, 0);
        actionLayout->setSpacing(kInstalledWaveformActionButtonSpacing);

        auto* restartBtn = new QPushButton(tr("Restart"), actionWidget);
        restartBtn->setAccessibleName(tr("Restart waveform %1").arg(name));
        connect(restartBtn, &QPushButton::clicked, this, [this, name]() {
            m_radioModel->flexWaveformModel().requestRestart(name);
        });

        // Remove / Uninstall button
        const QString removeLabel = isContainer ? tr("Remove") : tr("Uninstall");
        auto* removeBtn = new QPushButton(removeLabel, actionWidget);
        removeBtn->setAccessibleName(tr("%1 waveform %2").arg(removeLabel, name));
        connect(removeBtn, &QPushButton::clicked, this, [this, name, isContainer]() {
            if (!confirmRadioWaveformRemoval(this, name, isContainer)) {
                return;
            }
            if (isContainer) {
                m_radioModel->flexWaveformModel().requestRemoveContainer(name);
            } else {
                m_radioModel->flexWaveformModel().requestUninstall(name);
            }
        });

        const int actionButtonWidth = installedWaveformActionButtonWidth(restartBtn, removeBtn);
        restartBtn->setFixedWidth(actionButtonWidth);
        restartBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        removeBtn->setFixedWidth(actionButtonWidth);
        removeBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        actionWidget->setFixedWidth((actionButtonWidth * 2) + kInstalledWaveformActionButtonSpacing);
        actionLayout->addWidget(restartBtn);
        actionLayout->addWidget(removeBtn);
        rowLayout->addWidget(actionWidget, 0, Qt::AlignRight | Qt::AlignVCenter);

        m_listLayout->insertWidget(m_listLayout->count() - 1, row);
    }
}

} // namespace AetherSDR
