#include "CallsignCard.h"

#include "core/ThemeManager.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QImageReader>
#include <QPixmap>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// QLabel that opens the station's QRZ page on click — gives the callsign a
// link affordance without pulling in QTextBrowser.  Keyboard-activatable
// per docs/a11y.md (interactive-QLabel anti-pattern): Space/Enter trigger.
class CallsignLinkLabel : public QLabel {
public:
    using QLabel::QLabel;

protected:
    void mouseReleaseEvent(QMouseEvent* ev) override
    {
        if (ev->button() == Qt::LeftButton)
            openQrzPage();
        QLabel::mouseReleaseEvent(ev);
    }
    void keyPressEvent(QKeyEvent* ev) override
    {
        if (ev->key() == Qt::Key_Space || ev->key() == Qt::Key_Return
            || ev->key() == Qt::Key_Enter) {
            openQrzPage();
            return;
        }
        QLabel::keyPressEvent(ev);
    }

private:
    void openQrzPage()
    {
        const QString call = text().trimmed();
        if (!call.isEmpty() && !call.contains(QLatin1Char(' ')))
            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://www.qrz.com/db/%1").arg(call)));
    }
};

// "2,412 mi @ 078°" / "3 881 km @ 078°" from the operator's position;
// "~" marks a DXCC-country-center estimate.  Locale picks the unit.
QString formatDistanceBearing(double km, double bearingDeg, bool approx)
{
    const bool imperial =
        QLocale().measurementSystem() != QLocale::MetricSystem;
    const double value = imperial ? km * 0.621371 : km;
    const QString unit = imperial ? QStringLiteral("mi") : QStringLiteral("km");
    return QStringLiteral("%1%L2 %3 @ %4°")
        .arg(approx ? QStringLiteral("~") : QString())
        .arg(qRound(value))
        .arg(unit)
        .arg(qRound(bearingDeg), 3, 10, QLatin1Char('0'));
}

// Rounded-corner copy of a station photo scaled to fill edge×edge.
QPixmap roundedPhoto(const QPixmap& src, int edge, int radius)
{
    const QPixmap scaled = src.scaled(edge, edge, Qt::KeepAspectRatioByExpanding,
                                      Qt::SmoothTransformation);
    QPixmap out(edge, edge);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(0, 0, edge, edge, radius, radius);
    p.setClipPath(path);
    const int x = (edge - scaled.width()) / 2;
    const int y = (edge - scaled.height()) / 2;
    p.drawPixmap(x, y, scaled);
    return out;
}

} // namespace

CallsignCard::CallsignCard(Variant variant, QWidget* parent)
    : QFrame(parent), m_variant(variant)
{
    const bool large = (m_variant == Variant::Large);
    setObjectName(large ? QStringLiteral("callsignCardLarge")
                        : QStringLiteral("callsignCard"));
    setAccessibleName(QStringLiteral("Station contact card"));
    setAccessibleDescription(
        QStringLiteral("Callsign, name, and location of the looked-up station"));

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // The colored vertical line on the left edge — the card's signature.
    m_accentBar = new QWidget(this);
    m_accentBar->setFixedWidth(large ? 5 : 4);
    outer->addWidget(m_accentBar);

    auto* body = new QHBoxLayout;
    const int pad = large ? 12 : 6;
    body->setContentsMargins(pad, pad, pad, pad);
    body->setSpacing(large ? 12 : 8);
    outer->addLayout(body, 1);

    m_photoLabel = new QLabel(this);
    m_photoLabel->setFixedSize(photoEdge(), photoEdge());
    m_photoLabel->setAlignment(Qt::AlignCenter);
    m_photoLabel->setAccessibleName(QStringLiteral("Station photo"));
    body->addWidget(m_photoLabel, 0, Qt::AlignTop);

    auto* details = new QVBoxLayout;
    details->setSpacing(large ? 3 : 1);
    body->addLayout(details, 1);

    auto* callRow = new QHBoxLayout;
    callRow->setSpacing(6);
    m_callLabel = new CallsignLinkLabel(this);
    m_callLabel->setObjectName(QStringLiteral("callsignCardCall"));
    m_callLabel->setAccessibleName(QStringLiteral("Callsign"));
    m_callLabel->setCursor(Qt::PointingHandCursor);
    m_callLabel->setFocusPolicy(Qt::TabFocus);
    m_callLabel->setToolTip(QStringLiteral("Open this station's QRZ.com page"));
    callRow->addWidget(m_callLabel);

    m_classChip = new QLabel(this);
    m_classChip->setObjectName(QStringLiteral("callsignCardClass"));
    m_classChip->setAccessibleName(QStringLiteral("License class"));
    callRow->addWidget(m_classChip);

    m_cacheHint = new QLabel(this);
    m_cacheHint->setObjectName(QStringLiteral("callsignCardCacheHint"));
    callRow->addWidget(m_cacheHint);
    callRow->addStretch();

    m_closeBtn = new QPushButton(QStringLiteral("✕"), this);
    m_closeBtn->setObjectName(QStringLiteral("callsignCardClose"));
    m_closeBtn->setAccessibleName(QStringLiteral("Close contact card"));
    m_closeBtn->setFixedSize(16, 16);
    m_closeBtn->setToolTip(QStringLiteral("Hide the contact card"));
    m_closeBtn->setVisible(false);
    connect(m_closeBtn, &QPushButton::clicked, this, &CallsignCard::closeRequested);
    callRow->addWidget(m_closeBtn, 0, Qt::AlignTop);
    details->addLayout(callRow);

    m_nameLabel = new QLabel(this);
    m_nameLabel->setObjectName(QStringLiteral("callsignCardName"));
    m_nameLabel->setAccessibleName(QStringLiteral("Operator name"));
    details->addWidget(m_nameLabel);

    m_locationLabel = new QLabel(this);
    m_locationLabel->setObjectName(QStringLiteral("callsignCardLocation"));
    m_locationLabel->setAccessibleName(QStringLiteral("Location"));
    details->addWidget(m_locationLabel);

    m_metaLabel = new QLabel(this);
    m_metaLabel->setObjectName(QStringLiteral("callsignCardMeta"));
    m_metaLabel->setAccessibleName(QStringLiteral("Grid and details"));
    details->addWidget(m_metaLabel);

    details->addStretch();

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &CallsignCard::applyTheme);

    clearCard();
}

int CallsignCard::photoEdge() const
{
    return m_variant == Variant::Large ? 96 : 56;
}

void CallsignCard::applyTheme()
{
    const bool large = (m_variant == Variant::Large);
    auto& tm = ThemeManager::instance();

    tm.applyStyleSheet(this,
        QStringLiteral("QFrame#%1 { background: {{color.background.1}};"
                       " border: 1px solid {{color.border.strong}};"
                       " border-radius: 4px; }").arg(objectName()));
    tm.applyStyleSheet(m_accentBar,
        "QWidget { background: {{color.accent}};"
        " border-top-left-radius: 4px; border-bottom-left-radius: 4px; }");
    tm.applyStyleSheet(m_callLabel,
        QStringLiteral("QLabel { color: {{color.accent.bright}}; font-size: %1px;"
                       " font-weight: bold; background: transparent; border: none; }"
                       "QLabel:focus { text-decoration: underline; }")
            .arg(large ? 22 : 15));
    tm.applyStyleSheet(m_classChip,
        QStringLiteral("QLabel { color: {{color.accent}};"
                       " border: 1px solid {{color.accent.dim}}; border-radius: 3px;"
                       " padding: 0px 4px; font-size: %1px; background: transparent; }")
            .arg(large ? 11 : 9));
    tm.applyStyleSheet(m_cacheHint,
        QStringLiteral("QLabel { color: {{color.text.label}}; font-size: %1px;"
                       " background: transparent; border: none; }").arg(large ? 10 : 8));
    tm.applyStyleSheet(m_nameLabel,
        QStringLiteral("QLabel { color: {{color.text.primary}}; font-size: %1px;"
                       " background: transparent; border: none; }").arg(large ? 15 : 12));
    tm.applyStyleSheet(m_locationLabel,
        QStringLiteral("QLabel { color: {{color.text.secondary}}; font-size: %1px;"
                       " background: transparent; border: none; }").arg(large ? 13 : 11));
    tm.applyStyleSheet(m_metaLabel,
        QStringLiteral("QLabel { color: {{color.text.label}}; font-size: %1px;"
                       " background: transparent; border: none; }").arg(large ? 12 : 10));
    tm.applyStyleSheet(m_photoLabel,
        QStringLiteral("QLabel { background: {{color.background.2}};"
                       " border: none; border-radius: 6px; color: {{color.text.label}};"
                       " font-size: %1px; }").arg(photoEdge() / 2));
    tm.applyStyleSheet(m_closeBtn,
        "QPushButton { background: transparent; color: {{color.text.label}};"
        " border: none; font-size: 10px; }"
        "QPushButton:hover { color: {{color.accent.danger}}; }");
}

void CallsignCard::setPlaceholderPhoto()
{
    // Person-silhouette glyph on the themed placeholder background.
    m_photoLabel->setPixmap(QPixmap());
    m_photoLabel->setText(QStringLiteral("\U0001F464"));  // 👤
}

void CallsignCard::setCloseButtonVisible(bool visible)
{
    m_closeBtn->setVisible(visible);
}

void CallsignCard::clearCard()
{
    m_call.clear();
    m_callLabel->setText({});
    m_classChip->setVisible(false);
    m_cacheHint->setVisible(false);
    m_nameLabel->setText({});
    m_locationLabel->setText({});
    m_metaLabel->setText({});
    setPlaceholderPhoto();
}

void CallsignCard::showPending(const QString& call)
{
    clearCard();
    m_call = call;
    m_callLabel->setText(call);
    m_nameLabel->setText(QStringLiteral("Looking up…"));
}

void CallsignCard::showError(const QString& call, const QString& message)
{
    clearCard();
    m_call = call;
    m_callLabel->setText(call);
    m_nameLabel->setText(message);
}

void CallsignCard::showInfo(const CallsignInfo& info, bool fromCache)
{
    m_call = info.call;
    m_callLabel->setText(info.call);

    m_classChip->setText(info.licenseClass);
    m_classChip->setVisible(!info.licenseClass.isEmpty());
    // One provenance hint slot: "cached" (7-day cache) or "prefix"
    // (country-level cty.dat stand-in while QRZ is unreachable/slow).
    m_cacheHint->setText(info.prefixOnly ? QStringLiteral("prefix")
                         : fromCache     ? QStringLiteral("cached")
                                         : QString());
    m_cacheHint->setVisible(info.prefixOnly || fromCache);

    // A prefix card has no operator name; the country line carries it.
    m_nameLabel->setText(info.prefixOnly ? QString() : info.displayName());
    m_locationLabel->setText(info.displayLocation());

    QStringList meta;
    if (!info.grid.isEmpty())   meta << info.grid;
    if (!info.county.isEmpty()) meta << info.county;
    if (info.prefixOnly) {
        if (!info.continent.isEmpty()) meta << info.continent;
        if (info.cqZone > 0) meta << QStringLiteral("CQ %1").arg(info.cqZone);
    }
    if (info.distanceKm >= 0.0)
        meta << formatDistanceBearing(info.distanceKm, info.bearingDeg,
                                      info.distanceApprox);
    if (m_variant == Variant::Large) {
        QStringList qsl;
        if (info.lotw)    qsl << QStringLiteral("LoTW");
        if (info.eqsl)    qsl << QStringLiteral("eQSL");
        if (info.mailQsl) qsl << QStringLiteral("QSL");
        if (!qsl.isEmpty()) meta << qsl.join(QLatin1Char('/'));
    }
    m_metaLabel->setText(meta.join(QStringLiteral(" · ")));

    setPlaceholderPhoto();
}

void CallsignCard::setPhotoPath(const QString& imagePath)
{
    // Check dimensions before decoding so an image with a small file but huge
    // pixel dimensions (decompression bomb) can't freeze/OOM the GUI. (#3990)
    QImageReader reader(imagePath);
    const QSize dim = reader.size();
    if (dim.isValid() && (dim.width() > 4096 || dim.height() > 4096)) {
        setPlaceholderPhoto();
        return;
    }
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) {
        setPlaceholderPhoto();
        return;
    }
    m_photoLabel->setText({});
    m_photoLabel->setPixmap(roundedPhoto(QPixmap::fromImage(img), photoEdge(), 6));
}

} // namespace AetherSDR
