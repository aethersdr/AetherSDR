#include "CrossNeedleMeterApplet.h"

#include "CrossNeedleMeterSettings.h"
#include "CrossNeedleMeterWidget.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "core/ThemeManager.h"

#include <QAction>
#include <QActionGroup>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace AetherSDR {

namespace {

// Open the popped-out meter large enough to keep the dense physical scale
// readable, but let users shrink it to the widget's natural minimum. Keep
// these at the face's 3:2 aspect ratio; the floating window adds its title bar
// above this content.
constexpr int kFloatingWidth = 640;
constexpr int kFloatingHeight = 427;
constexpr int kFloatingMinimumWidth = 210;
constexpr int kFloatingMinimumHeight = 140;

} // namespace

namespace MeterSettings = CrossNeedleMeterSettingsCodec;

CrossNeedleMeterApplet::CrossNeedleMeterApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/power"));
    setObjectName(QStringLiteral("powerAndSwrApplet"));
    setAccessibleName(tr("Power and SWR applet"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_meter = new CrossNeedleMeterWidget(this);
    m_meter->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_meter);

    loadSettings();
    connect(m_meter, &QWidget::customContextMenuRequested,
            this, &CrossNeedleMeterApplet::showContextMenu);
}

QSize CrossNeedleMeterApplet::sizeHint() const
{
    if (m_floating) {
        return QSize(kFloatingWidth, kFloatingHeight);
    }
    return QWidget::sizeHint();
}

QSize CrossNeedleMeterApplet::minimumSizeHint() const
{
    if (m_floating) {
        return QSize(kFloatingMinimumWidth, kFloatingMinimumHeight);
    }
    return QWidget::minimumSizeHint();
}

void CrossNeedleMeterApplet::setFloating(bool floating)
{
    if (m_floating == floating) {
        return;
    }
    m_floating = floating;

    setSizePolicy(QSizePolicy::Preferred,
                  floating ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    m_meter->setSizePolicy(QSizePolicy::Expanding,
                           floating ? QSizePolicy::Expanding : QSizePolicy::Preferred);
    setProperty("floatingLayoutActive", floating);
    updateGeometry();
    m_meter->updateGeometry();
}

void CrossNeedleMeterApplet::setTxMeters(float forwardWatts, float swr)
{
    m_meter->setTxMeters(forwardWatts, swr);
}

void CrossNeedleMeterApplet::setTxPowers(float forwardWatts, float reflectedWatts)
{
    m_meter->setTxPowers(forwardWatts, reflectedWatts);
}

void CrossNeedleMeterApplet::setTransmitting(bool transmitting)
{
    m_meter->setTransmitting(transmitting);
}

void CrossNeedleMeterApplet::setPowerScale(int maxWatts, bool amplifierActive)
{
    m_meter->setPowerScale(maxWatts, amplifierActive);
}

void CrossNeedleMeterApplet::loadSettings()
{
    const QString raw = AppSettings::instance()
                            .value(MeterSettings::kSettingsKey, QString())
                            .toString();
    MeterSettings::Snapshot settings;
    bool rewriteSettings = false;
    if (!raw.isEmpty()) {
        QString error;
        settings = MeterSettings::decode(raw.toUtf8(), &error);
        if (!error.isEmpty()) {
            qCWarning(lcGui).noquote()
                << "CrossNeedleMeterApplet: ignoring invalid settings:" << error;
        } else {
            const int storedVersion =
                QJsonDocument::fromJson(raw.toUtf8()).object()
                    .value(QStringLiteral("version")).toInt();
            rewriteSettings = storedVersion < MeterSettings::kVersion;
        }
    }
    setFaceTheme(settings.faceTheme, false);
    setRangeLegendVisible(settings.showRange, false);
    if (rewriteSettings) {
        persistSettings();
    }
}

void CrossNeedleMeterApplet::persistSettings() const
{
    MeterSettings::Snapshot settings;
    settings.faceTheme = m_faceTheme;
    settings.showRange = m_rangeLegendVisible;
    AppSettings& appSettings = AppSettings::instance();
    appSettings.setValue(MeterSettings::kSettingsKey,
                         MeterSettings::encode(settings));
    appSettings.save();
}

void CrossNeedleMeterApplet::setFaceTheme(const QString& theme, bool persist)
{
    m_faceTheme = MeterSettings::normalizeTheme(theme);
    CrossNeedleMeterWidget::FaceTheme faceTheme =
        CrossNeedleMeterWidget::FaceTheme::DarkRoomUplight;
    if (m_faceTheme == MeterSettings::kClassicTheme) {
        faceTheme = CrossNeedleMeterWidget::FaceTheme::ClassicWarm;
    } else if (m_faceTheme == MeterSettings::kDarkTheme) {
        faceTheme = CrossNeedleMeterWidget::FaceTheme::GraphiteDark;
    }
    m_meter->setFaceTheme(faceTheme);
    setProperty("crossNeedleFaceTheme", m_faceTheme);
    if (persist) {
        persistSettings();
    }
}

void CrossNeedleMeterApplet::setRangeLegendVisible(bool visible, bool persist)
{
    m_rangeLegendVisible = visible;
    m_meter->setRangeLegendVisible(visible);
    setProperty("crossNeedleRangeLegendVisible", visible);
    if (persist) {
        persistSettings();
    }
}

void CrossNeedleMeterApplet::showContextMenu(const QPoint& position)
{
    QMenu menu(m_meter);

    auto addHeader = [&menu](const QString& text) {
        auto* label = new QLabel(text);
        label->setStyleSheet(
            "color:#8090a0; font-size:10px; font-weight:bold; "
            "padding:4px 8px 2px 8px;");
        auto* action = new QWidgetAction(&menu);
        action->setDefaultWidget(label);
        action->setEnabled(false);
        menu.addAction(action);
    };

    addHeader(QStringLiteral("Face theme"));
    auto* faceThemeGroup = new QActionGroup(&menu);

    QAction* classicThemeAction = menu.addAction(QStringLiteral("Classic warm"));
    classicThemeAction->setObjectName(
        QStringLiteral("crossNeedleClassicFaceThemeAction"));
    classicThemeAction->setCheckable(true);
    classicThemeAction->setChecked(m_faceTheme == MeterSettings::kClassicTheme);
    faceThemeGroup->addAction(classicThemeAction);
    connect(classicThemeAction, &QAction::triggered, this, [this]() {
        setFaceTheme(MeterSettings::kClassicTheme, true);
    });

    QAction* uplightThemeAction = menu.addAction(QStringLiteral("Dark-room uplight"));
    uplightThemeAction->setObjectName(
        QStringLiteral("crossNeedleUplightFaceThemeAction"));
    uplightThemeAction->setCheckable(true);
    uplightThemeAction->setChecked(m_faceTheme == MeterSettings::kUplightTheme);
    faceThemeGroup->addAction(uplightThemeAction);
    connect(uplightThemeAction, &QAction::triggered, this, [this]() {
        setFaceTheme(MeterSettings::kUplightTheme, true);
    });

    QAction* darkThemeAction = menu.addAction(QStringLiteral("Graphite dark"));
    darkThemeAction->setObjectName(QStringLiteral("crossNeedleDarkFaceThemeAction"));
    darkThemeAction->setCheckable(true);
    darkThemeAction->setChecked(m_faceTheme == MeterSettings::kDarkTheme);
    faceThemeGroup->addAction(darkThemeAction);
    connect(darkThemeAction, &QAction::triggered, this, [this]() {
        setFaceTheme(MeterSettings::kDarkTheme, true);
    });

    menu.addSeparator();
    addHeader(tr("Display"));
    QAction* showRangeAction = menu.addAction(tr("Show Range"));
    showRangeAction->setObjectName(QStringLiteral("crossNeedleShowRangeAction"));
    showRangeAction->setCheckable(true);
    showRangeAction->setChecked(m_rangeLegendVisible);
    connect(showRangeAction, &QAction::toggled, this, [this](bool checked) {
        setRangeLegendVisible(checked, true);
    });

    if (qEnvironmentVariableIsSet("AETHER_AUTOMATION")) {
        menu.addSeparator();
        addHeader(QStringLiteral("Automation proof"));
        QAction* activeProof =
            menu.addAction(QStringLiteral("Test 100 W / 4 W reflected"));
        activeProof->setObjectName(QStringLiteral("crossNeedleActiveProofAction"));
        connect(activeProof, &QAction::triggered, this, [this]() {
            m_meter->setPowerScale(200, false);
            m_meter->setAutomationPowerFixture(100.0f, 4.0f);
        });

        QAction* idleProof = menu.addAction(QStringLiteral("Clear test"));
        idleProof->setObjectName(QStringLiteral("crossNeedleIdleProofAction"));
        connect(idleProof, &QAction::triggered,
                m_meter, &CrossNeedleMeterWidget::clearAutomationFixture);
    }

    menu.exec(m_meter->mapToGlobal(position));
}

} // namespace AetherSDR
