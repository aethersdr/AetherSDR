#include "PropDashboardDialog.h"
#include "core/AppSettings.h"

#include <QColor>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QScrollArea>
#include <QScreen>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace MasterSDR {

namespace {

constexpr int kMetricKIndex = 0;
constexpr int kMetricAIndex = 1;
constexpr int kMetricSfi = 2;
constexpr int kMetricSunspots = 3;
constexpr int kMetricXray = 4;
constexpr int kTeachingBadgeWidth = 76;
constexpr int kSolarLunarImageSize = 148;

struct MetricDefinition {
    const char* title;
    const char* toolTip;
};

struct SolarImageType {
    const char* code;
    const char* label;
};

constexpr MetricDefinition kMetricDefinitions[] = {
    {"K INDEX", "K-index measures short-term geomagnetic disturbance on a scale from 0 to 9. Lower values usually mean steadier HF paths, while 5 and above can bring aurora and noisy polar paths."},
    {"A INDEX", "A-index is the all-day average of geomagnetic activity. Lower values suggest calmer conditions, while higher values mean the bands may stay unsettled even if the latest K-index looks quiet."},
    {"SOLAR FLUX", "Solar flux index estimates how much ionizing energy the sun is feeding into the upper atmosphere. Higher values usually help 15m, 12m, and 10m open longer and more often."},
    {"SUNSPOTS", "Sunspot number tracks how many active sunspot regions are facing Earth. More spots often means stronger ionization and better support for higher-frequency HF propagation."},
    {"X-RAY", "X-ray class shows how intense the latest solar flare activity is. Stronger C, M, and X-class flares can trigger daylight radio blackouts on sunlit paths."},
};

static constexpr SolarImageType kSolarTypes[] = {
    {"0193", "Corona (193\xc3\x85)"},
    {"0304", "Chromosphere (304\xc3\x85)"},
    {"0171", "Quiet Corona (171\xc3\x85)"},
    {"0094", "Flaring (94\xc3\x85)"},
    {"HMIIC", "Visible (HMI)"},
};
static constexpr int kSolarTypeCount = 5;

const QString kDialogStyle = QStringLiteral(
    "QDialog { background: #08121d; color: #d7e7f2; }"
    "QLabel { background: transparent; border: none; }"
    "QToolTip { color: #e8f4fb; background-color: #102131; border: 1px solid #355166; padding: 6px 8px; }");

const QString kGroupStyle = QStringLiteral(
    "QGroupBox {"
    " color: #d7e7f2;"
    " font-weight: 700;"
    " background: #0b1724;"
    " border: 1px solid #243a4e;"
    " border-radius: 12px;"
    " margin-top: 10px;"
    " padding-top: 16px;"
    "}"
    "QGroupBox::title {"
    " subcontrol-origin: margin;"
    " left: 12px;"
    " padding: 0 6px;"
    " color: #57c3e7;"
    "}");

const QString kLabelStyle = QStringLiteral(
    "QLabel { color: #8fa5b7; font-size: 11px; background: transparent; border: none; }");
const QString kHeaderLabelStyle = QStringLiteral(
    "QLabel { color: #d7e7f2; font-size: 11px; font-weight: 700; background: transparent; border: none; }");
const QString kValueStyle = QStringLiteral(
    "QLabel { color: #d7e7f2; font-weight: 700; font-size: 13px; background: transparent; border: none; }");
const QString kMutedStyle = QStringLiteral(
    "QLabel { color: #7f93a5; font-size: 10px; background: transparent; border: none; }");
const QString kLearnTitleStyle = QStringLiteral(
    "QLabel { color: #d7e7f2; font-size: 12px; font-weight: 700; background: transparent; border: none; }");
const QString kLearnBodyStyle = QStringLiteral(
    "QLabel { color: #a9bccb; font-size: 11px; line-height: 1.35; background: transparent; border: none; }");

QString tintedBackground(const QString& accent)
{
    QColor color(accent);
    color.setAlpha(40);
    return color.name(QColor::HexArgb);
}

QString pillStyle(const QString& accent)
{
    return QString(
        "QLabel {"
        " color: %1;"
        " font-weight: 700;"
        " font-size: 11px;"
        " background: %2;"
        " border: 1px solid %1;"
        " border-radius: 11px;"
        " padding: 4px 10px;"
        "}")
        .arg(accent, tintedBackground(accent));
}

QString metricCardStyle(const QString& accent)
{
    return QString(
        "QFrame#metricCard {"
        " background: #0d1b29;"
        " border: 1px solid %1;"
        " border-radius: 12px;"
        "}"
        "QLabel#metricTitle {"
        " color: #8fa5b7;"
        " font-size: 10px;"
        " font-weight: 700;"
        " background: transparent;"
        " border: none;"
        "}"
        "QLabel#metricValue {"
        " color: %1;"
        " font-size: 24px;"
        " font-weight: 800;"
        " background: transparent;"
        " border: none;"
        "}"
        "QLabel#metricDetail {"
        " color: #d7e7f2;"
        " font-size: 11px;"
        " background: transparent;"
        " border: none;"
        "}")
        .arg(accent);
}

QString forecastCellStyle(const QString& accent)
{
    return QString(
        "QLabel {"
        " color: %1;"
        " font-weight: 700;"
        " font-size: 11px;"
        " background: #101d2b;"
        " border: 1px solid #22384d;"
        " border-radius: 8px;"
        " padding: 3px 8px;"
        "}")
        .arg(accent);
}

QString compactBadgeStyle(const QString& accent)
{
    return QString(
        "QLabel {"
        " color: %1;"
        " font-weight: 700;"
        " font-size: 10px;"
        " background: %2;"
        " border: 1px solid %1;"
        " border-radius: 9px;"
        " padding: 3px 8px;"
        "}")
        .arg(accent, tintedBackground(accent));
}

QPixmap cropPixmapToVisibleSquare(const QPixmap& source, int threshold = 6, int padding = 6)
{
    if (source.isNull())
        return source;

    const QImage image = source.toImage().convertToFormat(QImage::Format_ARGB32);
    int minX = image.width();
    int minY = image.height();
    int maxX = -1;
    int maxY = -1;

    for (int y = 0; y < image.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = row[x];
            const int alpha = qAlpha(pixel);
            const int brightness = qMax(qMax(qRed(pixel), qGreen(pixel)), qBlue(pixel));
            if (alpha > threshold && brightness > threshold) {
                minX = qMin(minX, x);
                minY = qMin(minY, y);
                maxX = qMax(maxX, x);
                maxY = qMax(maxY, y);
            }
        }
    }

    if (maxX < minX || maxY < minY)
        return source;

    QRect bounds(QPoint(minX, minY), QPoint(maxX, maxY));
    bounds.adjust(-padding, -padding, padding, padding);
    bounds = bounds.intersected(image.rect());

    const int side = qMax(bounds.width(), bounds.height());
    const QPoint center = bounds.center();
    QRect square(center.x() - side / 2, center.y() - side / 2, side, side);

    if (square.left() < 0)
        square.moveLeft(0);
    if (square.top() < 0)
        square.moveTop(0);
    if (square.right() >= image.width())
        square.moveRight(image.width() - 1);
    if (square.bottom() >= image.height())
        square.moveBottom(image.height() - 1);
    square = square.intersected(image.rect());

    return source.copy(square);
}

QString kIndexSummary(double kp)
{
    if (kp >= 5.0) return "Storm-level geomagnetic activity";
    if (kp >= 3.0) return "Unsettled geomagnetic field";
    return "Quiet geomagnetic field";
}

QString aIndexColor(int aIndex)
{
    if (aIndex >= 30) return "#ff6b6b";
    if (aIndex >= 15) return "#f2c14e";
    return "#66d19e";
}

QString aIndexSummary(int aIndex)
{
    if (aIndex >= 30) return "Storm energy staying elevated";
    if (aIndex >= 15) return "Average disturbance is elevated";
    return "Daily average remains calm";
}

QString sfiColor(int sfi)
{
    if (sfi >= 170) return "#66d19e";
    if (sfi >= 120) return "#8fd27a";
    if (sfi > 0) return "#f2c14e";
    return "#7f93a5";
}

QString sfiSummary(int sfi)
{
    if (sfi >= 170) return "Upper HF is strongly favored";
    if (sfi >= 120) return "Upper HF is improving";
    if (sfi > 0) return "Lower bands likely lead today";
    return "Awaiting solar flux data";
}

QString sunspotColor(int sunspots)
{
    if (sunspots >= 150) return "#66d19e";
    if (sunspots >= 75) return "#8fd27a";
    if (sunspots >= 0) return "#f2c14e";
    return "#7f93a5";
}

QString sunspotSummary(int sunspots)
{
    if (sunspots >= 150) return "Busy solar disk with many regions";
    if (sunspots >= 75) return "Several active regions on the disk";
    if (sunspots >= 0) return "Few active regions in view";
    return "Awaiting sunspot data";
}

QString xrayColor(const QString& xrayClass)
{
    if (xrayClass.startsWith('X') || xrayClass.startsWith('M')) return "#ff6b6b";
    if (xrayClass.startsWith('C')) return "#f2c14e";
    if (!xrayClass.isEmpty()) return "#66d19e";
    return "#7f93a5";
}

QString xraySummary(const QString& xrayClass)
{
    if (xrayClass.startsWith('X') || xrayClass.startsWith('M')) return "Major flare energy is elevated";
    if (xrayClass.startsWith('C')) return "Minor flare activity is present";
    if (!xrayClass.isEmpty()) return "Solar X-ray flux is quiet";
    return "Awaiting X-ray data";
}

QString bandColor(const QString& condition)
{
    if (condition == "Good") return "#66d19e";
    if (condition == "Fair") return "#f2c14e";
    if (condition == "Poor") return "#ff8c6b";
    return "#7f93a5";
}

QString geomagFieldColor(const QString& field)
{
    if (field.contains("STORM", Qt::CaseInsensitive)) return "#ff6b6b";
    if (field.contains("ACTIVE", Qt::CaseInsensitive)) return "#f2c14e";
    if (field.contains("UNSETTLED", Qt::CaseInsensitive)) return "#e8b955";
    if (!field.isEmpty()) return "#66d19e";
    return "#7f93a5";
}

QString noiseColor(const QString& noise)
{
    if (noise.contains("S4") || noise.contains("S5") || noise.contains("S6") ||
        noise.contains("S7") || noise.contains("S8") || noise.contains("S9")) {
        return "#ff8c6b";
    }
    if (noise.contains("S2") || noise.contains("S3")) return "#f2c14e";
    if (!noise.isEmpty()) return "#66d19e";
    return "#7f93a5";
}

QString solarWindColor(double wind)
{
    if (wind >= 650.0) return "#ff8c6b";
    if (wind >= 500.0) return "#f2c14e";
    if (wind > 0.0) return "#66d19e";
    return "#7f93a5";
}

QString probabilityColor(int probability)
{
    if (probability >= 30) return "#ff6b6b";
    if (probability >= 10) return "#f2c14e";
    return "#66d19e";
}

QString vhfColor(const QString& condition)
{
    if (condition.contains("Open", Qt::CaseInsensitive)) return "#66d19e";
    if (!condition.isEmpty()) return "#7f93a5";
    return "#7f93a5";
}

void applyToolTip(QWidget* widget, const QString& toolTip)
{
    widget->setToolTip(toolTip);
    const auto children = widget->findChildren<QWidget*>();
    for (auto* child : children)
        child->setToolTip(toolTip);
}

} // namespace

QString PropDashboardDialog::kpColor(double kp)
{
    if (kp >= 5.0) return "#ff6b6b";
    if (kp >= 3.0) return "#f2c14e";
    return "#66d19e";
}

PropDashboardDialog::PropDashboardDialog(PropForecastClient* client, QWidget* parent)
    : PersistentDialog(QStringLiteral("HF Propagation Dashboard"),
                       QStringLiteral("PropDashboardDialogGeometry"), parent),
      m_client(client)
{
    setStyleSheet(kDialogStyle);

    const QScreen* screen = parentWidget() && parentWidget()->screen()
        ? parentWidget()->screen()
        : QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const int minWidth = qMax(760, qMin(920, available.width() - 40));
    const int minHeight = qMax(620, qMin(700, available.height() - 40));
    const int initialWidth = qMax(minWidth, qMin(1180, available.width() - 60));
    const int initialHeight = qMax(minHeight, qMin(820, available.height() - 60));
    setMinimumSize(minWidth, minHeight);
    resize(initialWidth, initialHeight);

    m_nam = new QNetworkAccessManager(this);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(scroll);

    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* mainLayout = new QVBoxLayout(content);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(10);

    auto makeTableValue = [](const QString& init = QStringLiteral("\u2014")) {
        auto* label = new QLabel(init);
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumWidth(62);
        label->setStyleSheet(kValueStyle);
        return label;
    };

    auto makePillValue = [](const QString& init = QStringLiteral("\u2014")) {
        auto* label = new QLabel(init);
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumHeight(28);
        label->setStyleSheet(pillStyle("#7f93a5"));
        return label;
    };

    {
        auto* group = new QGroupBox("Current Conditions");
        group->setStyleSheet(kGroupStyle);
        auto* v = new QVBoxLayout(group);
        v->setContentsMargins(12, 18, 12, 12);
        v->setSpacing(8);

        auto* cards = new QHBoxLayout;
        cards->setSpacing(10);
        for (int i = 0; i < kMetricCardCount; ++i) {
            auto* card = new QFrame;
            card->setObjectName("metricCard");
            card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            card->setMinimumHeight(108);
            card->setStyleSheet(metricCardStyle("#7f93a5"));

            auto* cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(12, 10, 12, 10);
            cardLayout->setSpacing(3);

            auto* title = new QLabel(kMetricDefinitions[i].title);
            title->setObjectName("metricTitle");
            title->setAlignment(Qt::AlignCenter);

            m_metricValueLabels[i] = new QLabel(QStringLiteral("\u2014"));
            m_metricValueLabels[i]->setObjectName("metricValue");
            m_metricValueLabels[i]->setAlignment(Qt::AlignCenter);

            m_metricDetailLabels[i] = new QLabel("Awaiting data");
            m_metricDetailLabels[i]->setObjectName("metricDetail");
            m_metricDetailLabels[i]->setAlignment(Qt::AlignCenter);
            m_metricDetailLabels[i]->setWordWrap(true);

            cardLayout->addWidget(title);
            cardLayout->addStretch();
            cardLayout->addWidget(m_metricValueLabels[i]);
            cardLayout->addWidget(m_metricDetailLabels[i]);
            cardLayout->addStretch();

            applyToolTip(card, QString::fromUtf8(kMetricDefinitions[i].toolTip));

            m_metricCards[i] = card;
            cards->addWidget(card, 1);
        }

        auto* hint = new QLabel("Hover any card for a quick plain-language explanation.");
        hint->setStyleSheet(kMutedStyle);

        v->addLayout(cards);
        v->addWidget(hint);
        mainLayout->addWidget(group);
    }

    auto* row1 = new QHBoxLayout;
    row1->setSpacing(10);

    {
        auto* group = new QGroupBox("3-Day Forecast");
        group->setStyleSheet(kGroupStyle);
        auto* g = new QGridLayout(group);
        g->setContentsMargins(12, 18, 12, 12);
        g->setVerticalSpacing(6);
        g->setHorizontalSpacing(8);

        auto* timeHdr = new QLabel("UTC");
        timeHdr->setStyleSheet(kHeaderLabelStyle);
        g->addWidget(timeHdr, 0, 0);

        for (int d = 0; d < 3; ++d) {
            m_dayHeaders[d] = new QLabel(QStringLiteral("\u2014"));
            m_dayHeaders[d]->setAlignment(Qt::AlignCenter);
            m_dayHeaders[d]->setStyleSheet(forecastCellStyle("#d7e7f2"));
            g->addWidget(m_dayHeaders[d], 0, d + 1);
        }

        static const char* periods[] = {
            "00-03", "03-06", "06-09", "09-12",
            "12-15", "15-18", "18-21", "21-00"
        };
        for (int p = 0; p < 8; ++p) {
            auto* lbl = new QLabel(periods[p]);
            lbl->setStyleSheet(kLabelStyle);
            g->addWidget(lbl, p + 1, 0);
            for (int d = 0; d < 3; ++d) {
                m_kpCells[d][p] = makeTableValue();
                m_kpCells[d][p]->setStyleSheet(forecastCellStyle("#d7e7f2"));
                g->addWidget(m_kpCells[d][p], p + 1, d + 1);
            }
        }

        int row = 9;
        auto* maxLbl = new QLabel("Max Kp");
        maxLbl->setStyleSheet(kHeaderLabelStyle);
        g->addWidget(maxLbl, row, 0);
        for (int d = 0; d < 3; ++d) {
            m_maxKpLabels[d] = makePillValue();
            g->addWidget(m_maxKpLabels[d], row, d + 1);
        }

        ++row;
        auto* sep = new QLabel;
        sep->setFixedHeight(1);
        sep->setStyleSheet("QLabel { background: #243a4e; }");
        g->addWidget(sep, row, 0, 1, 4);

        ++row;
        auto* r1Lbl = new QLabel("R1-R2");
        r1Lbl->setStyleSheet(kLabelStyle);
        g->addWidget(r1Lbl, row, 0);
        for (int d = 0; d < 3; ++d) {
            m_r1r2Labels[d] = makePillValue();
            g->addWidget(m_r1r2Labels[d], row, d + 1);
        }

        ++row;
        auto* r3Lbl = new QLabel("R3+");
        r3Lbl->setStyleSheet(kLabelStyle);
        g->addWidget(r3Lbl, row, 0);
        for (int d = 0; d < 3; ++d) {
            m_r3Labels[d] = makePillValue();
            g->addWidget(m_r3Labels[d], row, d + 1);
        }

        ++row;
        auto* s1Lbl = new QLabel("S1+");
        s1Lbl->setStyleSheet(kLabelStyle);
        g->addWidget(s1Lbl, row, 0);
        for (int d = 0; d < 3; ++d) {
            m_s1Labels[d] = makePillValue();
            g->addWidget(m_s1Labels[d], row, d + 1);
        }

        ++row;
        m_rationale = new QLabel;
        m_rationale->setWordWrap(true);
        m_rationale->setText("Forecast rationale will appear when NOAA publishes the current discussion.");
        m_rationale->setStyleSheet(
            "QLabel {"
            " color: #9cb0c0;"
            " font-size: 10px;"
            " background: #0f1d2b;"
            " border: 1px solid #243a4e;"
            " border-radius: 10px;"
            " padding: 8px;"
            "}");
        g->addWidget(m_rationale, row, 0, 1, 4);

        row1->addWidget(group, 3);
    }

    {
        auto* group = new QGroupBox("Solar And Lunar");
        group->setStyleSheet(kGroupStyle);
        auto* v = new QVBoxLayout(group);
        v->setContentsMargins(12, 18, 12, 12);
        v->setSpacing(10);

        auto* imagesRow = new QHBoxLayout;
        imagesRow->setSpacing(12);

        auto* solarCol = new QVBoxLayout;
        solarCol->setSpacing(6);
        m_solarImage = new QLabel;
        m_solarImage->setFixedSize(kSolarLunarImageSize, kSolarLunarImageSize);
        m_solarImage->setAlignment(Qt::AlignCenter);
        m_solarImage->setStyleSheet("QLabel { background: #02070d; border: 1px solid #22374a; border-radius: 10px; }");
        m_solarImage->setCursor(Qt::PointingHandCursor);
        m_solarImage->setToolTip("Click to cycle through solar images");
        m_solarImage->setText("Loading...");
        m_solarImage->installEventFilter(this);
        solarCol->addWidget(m_solarImage, 0, Qt::AlignCenter);

        m_solarTypeLabel = new QLabel("Corona (193\xc3\x85)");
        m_solarTypeLabel->setAlignment(Qt::AlignCenter);
        m_solarTypeLabel->setStyleSheet(kMutedStyle);
        solarCol->addWidget(m_solarTypeLabel);

        auto* lunarCol = new QVBoxLayout;
        lunarCol->setSpacing(6);
        m_lunarImage = new QLabel;
        m_lunarImage->setFixedSize(kSolarLunarImageSize, kSolarLunarImageSize);
        m_lunarImage->setAlignment(Qt::AlignCenter);
        m_lunarImage->setStyleSheet("QLabel { background: #02070d; border: 1px solid #22374a; border-radius: 10px; }");
        m_lunarImage->setText("Loading...");
        lunarCol->addWidget(m_lunarImage, 0, Qt::AlignCenter);

        m_lunarPhaseLabel = new QLabel;
        m_lunarPhaseLabel->setAlignment(Qt::AlignCenter);
        m_lunarPhaseLabel->setWordWrap(true);
        m_lunarPhaseLabel->setStyleSheet("QLabel { color: #f2d889; font-size: 10px; }");
        lunarCol->addWidget(m_lunarPhaseLabel);

        imagesRow->addLayout(solarCol, 1);
        imagesRow->addLayout(lunarCol, 1);

        auto* learnPanel = new QFrame;
        learnPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        learnPanel->setStyleSheet(
            "QFrame {"
            " background: #0e1926;"
            " border: 1px solid #22384d;"
            " border-radius: 10px;"
            "}");
        auto* learnLayout = new QVBoxLayout(learnPanel);
        learnLayout->setContentsMargins(10, 10, 10, 10);
        learnLayout->setSpacing(8);

        auto* learnTitle = new QLabel("What To Look For");
        learnTitle->setStyleSheet(kLearnTitleStyle);
        auto* learnHint = new QLabel("Click the solar image to cycle wavelengths and build intuition from different solar views.");
        learnHint->setWordWrap(true);
        learnHint->setStyleSheet(kMutedStyle);
        learnLayout->addWidget(learnTitle);
        learnLayout->addWidget(learnHint);

        static const char* noteBadges[kLearningNoteCount] = {"VIEW", "HF", "MOON"};
        static const char* noteColors[kLearningNoteCount] = {"#57c3e7", "#66d19e", "#f2d889"};
        for (int i = 0; i < kLearningNoteCount; ++i) {
            auto* row = new QHBoxLayout;
            row->setSpacing(8);
            row->setContentsMargins(0, 0, 0, 0);
            row->setAlignment(Qt::AlignTop);

            auto* badge = new QLabel(noteBadges[i]);
            badge->setAlignment(Qt::AlignCenter);
            badge->setFixedWidth(kTeachingBadgeWidth);
            badge->setStyleSheet(compactBadgeStyle(noteColors[i]));

            m_learningNoteLabels[i] = new QLabel("Loading...");
            m_learningNoteLabels[i]->setWordWrap(true);
            m_learningNoteLabels[i]->setStyleSheet(kLearnBodyStyle);

            row->addWidget(badge, 0, Qt::AlignTop);
            row->addWidget(m_learningNoteLabels[i], 1);
            learnLayout->addLayout(row);
        }
        learnLayout->addStretch();

        v->addLayout(imagesRow);
        v->addWidget(learnPanel, 1);

        row1->addWidget(group, 2);
    }

    mainLayout->addLayout(row1);

    auto* row2 = new QHBoxLayout;
    row2->setSpacing(10);

    {
        auto* group = new QGroupBox("HF Band Conditions");
        group->setStyleSheet(kGroupStyle);
        auto* g = new QGridLayout(group);
        g->setContentsMargins(12, 18, 12, 12);
        g->setVerticalSpacing(8);
        g->setHorizontalSpacing(12);

        static const char* bandNames[] = {"80m-40m", "30m-20m", "17m-15m", "12m-10m"};

        auto* dayHdr = new QLabel("Day");
        dayHdr->setAlignment(Qt::AlignCenter);
        dayHdr->setStyleSheet(kHeaderLabelStyle);
        g->addWidget(dayHdr, 0, 1);

        auto* nightHdr = new QLabel("Night");
        nightHdr->setAlignment(Qt::AlignCenter);
        nightHdr->setStyleSheet(kHeaderLabelStyle);
        g->addWidget(nightHdr, 0, 2);

        for (int i = 0; i < 4; ++i) {
            auto* lbl = new QLabel(bandNames[i]);
            lbl->setStyleSheet(kLabelStyle);
            g->addWidget(lbl, i + 1, 0);
            m_bandDayLabels[i] = makePillValue();
            g->addWidget(m_bandDayLabels[i], i + 1, 1);
            m_bandNightLabels[i] = makePillValue();
            g->addWidget(m_bandNightLabels[i], i + 1, 2);
        }

        int r = 5;
        auto addRow = [&](const char* name, QLabel*& label, const QString& tip) {
            auto* lbl = new QLabel(name);
            lbl->setStyleSheet(kLabelStyle);
            lbl->setToolTip(tip);
            g->addWidget(lbl, r, 0);
            label = makePillValue();
            label->setToolTip(tip);
            g->addWidget(label, r++, 1, 1, 2);
        };
        addRow("X-Ray", m_xrayLabel, "Latest solar X-ray flare class.");
        addRow("Solar Wind", m_windLabel, "Current solar wind speed at Earth.");
        addRow("Geomagnetic", m_geoFieldLabel, "N0NBH geomagnetic field assessment.");
        addRow("Noise", m_noiseLabel, "Expected atmospheric noise floor.");

        row2->addWidget(group, 3);
    }

    {
        auto* group = new QGroupBox("VHF Conditions");
        group->setStyleSheet(kGroupStyle);
        auto* v = new QVBoxLayout(group);
        v->setContentsMargins(12, 18, 12, 12);
        v->setSpacing(8);

        auto* g = new QGridLayout;
        g->setVerticalSpacing(8);
        g->setHorizontalSpacing(12);

        auto* aurLbl = new QLabel("Aurora");
        aurLbl->setStyleSheet(kLabelStyle);
        g->addWidget(aurLbl, 0, 0);
        m_auroraLabel = makePillValue();
        g->addWidget(m_auroraLabel, 0, 1);

        auto* esNaLbl = new QLabel("E-Skip NA");
        esNaLbl->setStyleSheet(kLabelStyle);
        g->addWidget(esNaLbl, 1, 0);
        m_esNaLabel = makePillValue();
        g->addWidget(m_esNaLabel, 1, 1);

        auto* esEuLbl = new QLabel("E-Skip EU");
        esEuLbl->setStyleSheet(kLabelStyle);
        g->addWidget(esEuLbl, 2, 0);
        m_esEuLabel = makePillValue();
        g->addWidget(m_esEuLabel, 2, 1);

        auto* note = new QLabel("Open usually means auroral or sporadic-E enhancement is more likely than usual.");
        note->setWordWrap(true);
        note->setStyleSheet(kMutedStyle);

        auto* learnPanel = new QFrame;
        learnPanel->setStyleSheet(
            "QFrame {"
            " background: #0e1926;"
            " border: 1px solid #22384d;"
            " border-radius: 10px;"
            "}");
        auto* learnLayout = new QVBoxLayout(learnPanel);
        learnLayout->setContentsMargins(10, 10, 10, 10);
        learnLayout->setSpacing(8);

        auto* learnTitle = new QLabel("What These Mean");
        learnTitle->setStyleSheet(kLearnTitleStyle);
        auto* learnHint = new QLabel("Aurora and sporadic-E are two very different reasons VHF can travel much farther than usual.");
        learnHint->setWordWrap(true);
        learnHint->setStyleSheet(kMutedStyle);
        learnLayout->addWidget(learnTitle);
        learnLayout->addWidget(learnHint);

        static const char* vhfBadges[kVhfNoteCount] = {"AURORA", "E-SKIP"};
        static const char* vhfColors[kVhfNoteCount] = {"#57c3e7", "#66d19e"};
        for (int i = 0; i < kVhfNoteCount; ++i) {
            auto* row = new QHBoxLayout;
            row->setSpacing(8);
            row->setContentsMargins(0, 0, 0, 0);
            row->setAlignment(Qt::AlignTop);

            auto* badge = new QLabel(vhfBadges[i]);
            badge->setAlignment(Qt::AlignCenter);
            badge->setFixedWidth(kTeachingBadgeWidth);
            badge->setStyleSheet(compactBadgeStyle(vhfColors[i]));

            m_vhfNoteLabels[i] = new QLabel("Loading...");
            m_vhfNoteLabels[i]->setWordWrap(true);
            m_vhfNoteLabels[i]->setStyleSheet(kLearnBodyStyle);

            row->addWidget(badge, 0, Qt::AlignTop);
            row->addWidget(m_vhfNoteLabels[i], 1);
            learnLayout->addLayout(row);
        }

        v->addLayout(g);
        v->addWidget(note);
        v->addWidget(learnPanel);
        v->addStretch();

        row2->addWidget(group, 2);
    }

    mainLayout->addLayout(row2);
    mainLayout->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    root->addWidget(buttons);

    connect(m_client, &PropForecastClient::detailUpdated,
            this, &PropDashboardDialog::onDetailUpdated);
    connect(m_client, &PropForecastClient::forecastUpdated,
            this, [this](const PropForecast&) { refresh(); });

    refresh();
    updateLearningNotes();
    updateVhfLearningNotes();
    m_client->fetchDetail();
    fetchImages();

    m_refreshTimer.setInterval(30 * 60 * 1000);
    connect(&m_refreshTimer, &QTimer::timeout, this, [this]() {
        m_client->fetchDetail();
        fetchImages();
    });
    m_refreshTimer.start();
}

void PropDashboardDialog::fetchImages()
{
    fetchSolarImage();

    QString ts = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm");
    QNetworkRequest lReq{QUrl{QString("https://svs.gsfc.nasa.gov/api/dialamoon/%1").arg(ts)}};
    lReq.setHeader(QNetworkRequest::UserAgentHeader, "MasterSDR");
    auto* lr = m_nam->get(lReq);
    connect(lr, &QNetworkReply::finished, this, [this, lr]() {
        lr->deleteLater();
        if (lr->error() != QNetworkReply::NoError) {
            m_lunarImage->setText("N/A");
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(lr->readAll());
        if (!doc.isObject()) return;
        QJsonObject obj = doc.object();

        QString phase = obj["curphase"].toString();
        int illum = static_cast<int>(obj["fracillum"].toString().toDouble());
        m_lunarPhaseName = phase;
        m_lunarIllumination = illum;
        const QString lunarCaption = phase.isEmpty()
            ? QString("%1% illuminated").arg(illum)
            : QString("%1\n%2% illuminated").arg(phase).arg(illum);
        m_lunarPhaseLabel->setText(lunarCaption);
        updateLearningNotes();

        QString imgUrl = obj["image"].toObject()["url"].toString();
        if (imgUrl.isEmpty()) return;

        QNetworkRequest ir{QUrl{imgUrl}};
        ir.setHeader(QNetworkRequest::UserAgentHeader, "MasterSDR");
        auto* imgR = m_nam->get(ir);
        connect(imgR, &QNetworkReply::finished, this, [this, imgR]() {
            imgR->deleteLater();
            if (imgR->error() == QNetworkReply::NoError) {
                QPixmap pm;
                if (pm.loadFromData(imgR->readAll())) {
                    const QPixmap cropped = cropPixmapToVisibleSquare(pm);
                    m_lunarImage->setPixmap(cropped.scaled(
                        kSolarLunarImageSize,
                        kSolarLunarImageSize,
                        Qt::KeepAspectRatio,
                        Qt::SmoothTransformation));
                }
            }
        });
    });
}

void PropDashboardDialog::refresh()
{
    const PropForecast fc = m_client->lastForecast();
    const PropForecastDetail& det = m_client->lastDetail();

    auto setCard = [this](int index, const QString& value, const QString& detail, const QString& accent) {
        m_metricValueLabels[index]->setText(value);
        m_metricDetailLabels[index]->setText(detail);
        m_metricCards[index]->setStyleSheet(metricCardStyle(accent));
    };

    if (fc.kIndex >= 0.0) {
        setCard(kMetricKIndex,
                QString::number(fc.kIndex, 'f', 2),
                kIndexSummary(fc.kIndex),
                kpColor(fc.kIndex));
    } else {
        setCard(kMetricKIndex, QStringLiteral("\u2014"), "Awaiting WWV update", "#7f93a5");
    }

    if (fc.aIndex >= 0) {
        setCard(kMetricAIndex,
                QString::number(fc.aIndex),
                aIndexSummary(fc.aIndex),
                aIndexColor(fc.aIndex));
    } else {
        setCard(kMetricAIndex, QStringLiteral("\u2014"), "Awaiting WWV update", "#7f93a5");
    }

    const int solarFlux = fc.sfi > 0 ? fc.sfi : det.solarFlux10cm;
    if (solarFlux > 0) {
        setCard(kMetricSfi,
                QString::number(solarFlux),
                sfiSummary(solarFlux),
                sfiColor(solarFlux));
    } else {
        setCard(kMetricSfi, QStringLiteral("\u2014"), "Awaiting solar flux data", "#7f93a5");
    }

    if (det.sunspotNumber >= 0) {
        setCard(kMetricSunspots,
                QString::number(det.sunspotNumber),
                sunspotSummary(det.sunspotNumber),
                sunspotColor(det.sunspotNumber));
    } else {
        setCard(kMetricSunspots, QStringLiteral("\u2014"), "Awaiting sunspot data", "#7f93a5");
    }

    if (!det.xrayClass.isEmpty()) {
        setCard(kMetricXray,
                det.xrayClass,
                xraySummary(det.xrayClass),
                xrayColor(det.xrayClass));
    } else {
        setCard(kMetricXray, QStringLiteral("\u2014"), "Awaiting X-ray data", "#7f93a5");
    }

    updateLearningNotes();
    updateVhfLearningNotes();
}

void PropDashboardDialog::onDetailUpdated(const PropForecastDetail& det)
{
    for (int d = 0; d < 3; ++d)
        m_dayHeaders[d]->setText(det.dayLabels[d]);

    for (int d = 0; d < 3; ++d) {
        for (int p = 0; p < 8; ++p) {
            double kp = det.kpForecast[d][p];
            m_kpCells[d][p]->setText(QString::number(kp, 'f', 2));
            m_kpCells[d][p]->setStyleSheet(forecastCellStyle(kpColor(kp)));
        }
        m_maxKpLabels[d]->setText(QString::number(det.maxKp[d], 'f', 2));
        m_maxKpLabels[d]->setStyleSheet(pillStyle(kpColor(det.maxKp[d])));
    }

    for (int d = 0; d < 3; ++d) {
        m_r1r2Labels[d]->setText(QString("%1%").arg(det.blackoutR1R2[d]));
        m_r1r2Labels[d]->setStyleSheet(pillStyle(probabilityColor(det.blackoutR1R2[d])));
        m_r3Labels[d]->setText(QString("%1%").arg(det.blackoutR3[d]));
        m_r3Labels[d]->setStyleSheet(pillStyle(probabilityColor(det.blackoutR3[d])));
        m_s1Labels[d]->setText(QString("%1%").arg(det.radiationS1[d]));
        m_s1Labels[d]->setStyleSheet(pillStyle(probabilityColor(det.radiationS1[d])));
    }

    QString rat;
    if (!det.geomagRationale.isEmpty())
        rat += det.geomagRationale;
    if (!det.blackoutRationale.isEmpty()) {
        if (!rat.isEmpty()) rat += " ";
        rat += det.blackoutRationale;
    }
    m_rationale->setText(rat.isEmpty()
        ? QStringLiteral("Forecast rationale will appear when NOAA publishes the current discussion.")
        : rat);

    for (int i = 0; i < 4; ++i) {
        if (!det.bandDay[i].isEmpty()) {
            m_bandDayLabels[i]->setText(det.bandDay[i]);
            m_bandDayLabels[i]->setStyleSheet(pillStyle(bandColor(det.bandDay[i])));
        }
        if (!det.bandNight[i].isEmpty()) {
            m_bandNightLabels[i]->setText(det.bandNight[i]);
            m_bandNightLabels[i]->setStyleSheet(pillStyle(bandColor(det.bandNight[i])));
        }
    }

    if (!det.xrayClass.isEmpty()) {
        m_xrayLabel->setText(det.xrayClass);
        m_xrayLabel->setStyleSheet(pillStyle(xrayColor(det.xrayClass)));
    }
    if (det.solarWind > 0) {
        m_windLabel->setText(QString("%1 km/s").arg(det.solarWind, 0, 'f', 1));
        m_windLabel->setStyleSheet(pillStyle(solarWindColor(det.solarWind)));
    }
    if (!det.geomagField.isEmpty()) {
        m_geoFieldLabel->setText(det.geomagField);
        m_geoFieldLabel->setStyleSheet(pillStyle(geomagFieldColor(det.geomagField)));
    }
    if (!det.signalNoise.isEmpty()) {
        m_noiseLabel->setText(det.signalNoise);
        m_noiseLabel->setStyleSheet(pillStyle(noiseColor(det.signalNoise)));
    }
    if (!det.vhfAurora.isEmpty()) {
        m_auroraLabel->setText(det.vhfAurora);
        m_auroraLabel->setStyleSheet(pillStyle(vhfColor(det.vhfAurora)));
    }
    if (!det.esNorthAmerica.isEmpty()) {
        m_esNaLabel->setText(det.esNorthAmerica);
        m_esNaLabel->setStyleSheet(pillStyle(vhfColor(det.esNorthAmerica)));
    }
    if (!det.esEurope.isEmpty()) {
        m_esEuLabel->setText(det.esEurope);
        m_esEuLabel->setStyleSheet(pillStyle(vhfColor(det.esEurope)));
    }

    refresh();
    updateVhfLearningNotes();
}

void PropDashboardDialog::updateLearningNotes()
{
    const PropForecast fc = m_client ? m_client->lastForecast() : PropForecast{};
    const PropForecastDetail det = m_client ? m_client->lastDetail() : PropForecastDetail{};

    QString viewNote;
    switch (m_solarTypeIndex % kSolarTypeCount) {
    case 0:
        viewNote = "In 193A, watch for dark coronal holes and bright active regions. Coronal holes often line up with faster solar wind reaching Earth a day or two later.";
        break;
    case 1:
        viewNote = "In 304A, look for bright plages and eruptive prominences around active regions. Sudden brightening here can go with increased flare activity.";
        break;
    case 2:
        viewNote = "In 171A, look for the shape and stability of coronal loops. Calm, simple loops usually mean less dramatic short-term change than tangled active regions.";
        break;
    case 3:
        viewNote = "In 94A, pay extra attention to bright hot cores inside active regions. This view is flare-sensitive, so intense patches deserve a closer look when X-ray levels rise.";
        break;
    default:
        viewNote = "In the visible HMI view, look for sunspot groups facing Earth. Large grouped spots often support stronger ionization but can also raise flare risk.";
        break;
    }

    const int solarFlux = fc.sfi > 0 ? fc.sfi : det.solarFlux10cm;
    const bool disturbedNow = fc.kIndex >= 5.0 || fc.aIndex >= 30
        || det.xrayClass.startsWith('X') || det.xrayClass.startsWith('M');
    QString hfNote;
    if (disturbedNow) {
        hfNote = "Right now the radio takeaway is caution because geomagnetic or flare activity is elevated. Expect noisier daylight or polar paths and try lower bands first when signals feel rough.";
    } else if (solarFlux >= 140 && fc.kIndex >= 0.0 && fc.kIndex < 3.0) {
        hfNote = "Right now upper HF has a better shot than usual. Start by checking 20m through 10m in daylight, then compare what you hear with the cards above.";
    } else {
        hfNote = "Treat the dashboard as a starting point, not a promise. Check 40m through 20m first, then move higher or lower based on real signals to build intuition.";
    }

    QString moonNote;
    if (!m_lunarPhaseName.isEmpty() && m_lunarIllumination >= 0) {
        moonNote = QString("%1 at %2% illumination is mostly a side note for ordinary HF. Moon phase matters much more for EME work, so use it here as context rather than a predictor of daily HF openings.")
            .arg(m_lunarPhaseName)
            .arg(m_lunarIllumination);
    } else {
        moonNote = "Moon phase is mostly a side note for ordinary HF. It matters much more for EME work than for everyday band openings.";
    }

    m_learningNoteLabels[0]->setText(viewNote);
    m_learningNoteLabels[1]->setText(hfNote);
    m_learningNoteLabels[2]->setText(moonNote);
}

void PropDashboardDialog::updateVhfLearningNotes()
{
    const PropForecastDetail det = m_client ? m_client->lastDetail() : PropForecastDetail{};

    QString auroraState = !det.vhfAurora.isEmpty() ? det.vhfAurora : QString("currently unknown");
    QString auroraNote = QString(
        "Aurora is %1. Aurora openings come from geomagnetic storms energizing high-latitude paths, and signals often sound hissy, rough, or fluttery compared with normal tropo or ground-wave VHF.")
        .arg(auroraState.toLower());

    const bool esOpen = det.esNorthAmerica.contains("Open", Qt::CaseInsensitive)
        || det.esEurope.contains("Open", Qt::CaseInsensitive);
    QString esLead = esOpen
        ? "Sporadic-E is active in at least one region right now."
        : "Sporadic-E is not indicated as open right now.";
    QString esNote = esLead
        + " Sporadic-E comes from dense E-layer patches that can make 6m, 10m, and sometimes 2m jump far beyond line of sight with sudden strong signals.";

    m_vhfNoteLabels[0]->setText(auroraNote);
    m_vhfNoteLabels[1]->setText(esNote);
}

void PropDashboardDialog::fetchSolarImage()
{
    const auto& type = kSolarTypes[m_solarTypeIndex % kSolarTypeCount];
    QString url = QString("https://sdo.gsfc.nasa.gov/assets/img/latest/latest_256_%1.jpg").arg(type.code);

    m_solarTypeLabel->setText(type.label);
    m_solarImage->setText("Loading...");
    updateLearningNotes();

    QNetworkRequest req{QUrl{url}};
    req.setHeader(QNetworkRequest::UserAgentHeader, "MasterSDR");
    auto* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            QPixmap pm;
            if (pm.loadFromData(reply->readAll()))
                m_solarImage->setPixmap(pm.scaled(kSolarLunarImageSize, kSolarLunarImageSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            m_solarImage->setText("Unavailable");
        }
    });
}

void PropDashboardDialog::cycleSolarImage()
{
    m_solarTypeIndex = (m_solarTypeIndex + 1) % kSolarTypeCount;
    fetchSolarImage();
}

bool PropDashboardDialog::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_solarImage && ev->type() == QEvent::MouseButtonPress) {
        cycleSolarImage();
        return true;
    }
    return QDialog::eventFilter(obj, ev);
}

} // namespace MasterSDR
