#include <QtGlobal>
#ifdef Q_OS_LINUX

#include "UlanziDialMapperDialog.h"
#include "core/AppSettings.h"
#include "core/EvdevEncoderManager.h"
#include "core/ShortcutManager.h"
#ifdef HAVE_MIDI
#include "core/MidiControlManager.h"
#endif

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace AetherSDR {

// Cached product image — loaded once, shared by dialBodyRect (for
// aspect-correct sizing) and the canvas paintEvent.  Falls back to a
// stylized painted body if the asset isn't present.
static QPixmap& ulanziDialPixmap()
{
    static QPixmap pm;
    static bool triedLoad = false;
    if (!triedLoad) {
        triedLoad = true;
        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList candidates = {
            appDir + QStringLiteral("/../resources/images/ulanzi-dial.png"),
            appDir + QStringLiteral("/resources/images/ulanzi-dial.png"),
            QStringLiteral(":/images/ulanzi-dial.png"),
        };
        for (const QString& path : candidates) {
            if (QFile::exists(path) || path.startsWith(QLatin1Char(':'))) {
                if (pm.load(path) && !pm.isNull()) break;
            }
        }
    }
    return pm;
}
static QSize ulanziDialImageSize()
{
    const QPixmap& pm = ulanziDialPixmap();
    return pm.isNull() ? QSize() : pm.size();
}

// Dedicated paint surface for the dial body + connector lines.  Lives
// inside the dialog's bodyWidget layout so we don't fight the global
// QSS that paints opaque backgrounds on bare QWidgets at the dialog
// level.  Pills are children of this canvas, so move() positions and
// paint coordinates share the same coordinate system.
class UlanziDialCanvas : public QWidget {
public:
    explicit UlanziDialCanvas(UlanziDialMapperDialog* owner)
        : QWidget(owner->bodyWidget()), m_owner(owner)
    {
        // Stop the global QSS from painting an opaque background here.
        setAttribute(Qt::WA_StyledBackground, false);
        setAutoFillBackground(false);
        setStyleSheet(QStringLiteral("background: transparent;"));
        setFixedSize(620, 555);  // matches the 640×640 fixed dialog
    }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    UlanziDialMapperDialog* m_owner;
};

namespace {

// Immutable signature ↔ pill mapping.  These bindings represent what
// physical button on the dial emits which kernel event — they're a
// property of the firmware, not user-configurable.  Verified against
// dial firmware 1.x: changing this table requires re-flashing the dial.
//
// Anchor coordinates match the product image (square body, 3 top
// buttons, large central knob, two tabs on each side).
struct PillSpec {
    const char* id;
    const char* label;          // physical-control label
    const char* signature;      // immutable kernel-event signature
    const char* defaultAction;  // initial function binding
    double nx, ny;              // anchor on dial body (0..1)
    int    side;                // 0 top, 1 right, 2 bottom, 3 left
};
// defaultAction must be one of the action IDs below (not the label) —
// loadActions calls findData() against the combo's stored data role.
// Default action IDs use the prefixed form ("shortcut:<id>") that
// MainWindow's dispatcher parses.  The signature ↔ pill mapping is a
// firmware property; the function bound to each pill is configurable.
// Rotary (vfo_tune) is intentionally absent — the rotary signal is
// routed by MainWindow directly, separate from the button mapper.
constexpr PillSpec kPillSpecs[] = {
    {"top_left",   "Top Left",   "KEY_PREVIOUSSONG",  "shortcut:mox_toggle",   0.21, 0.06, 0},
    {"top_middle", "Top Middle", "KEY_PLAYPAUSE",     "shortcut:rit_toggle",   0.50, 0.06, 0},
    {"top_right",  "Top Right",  "KEY_NEXTSONG",      "shortcut:tune_toggle",  0.79, 0.06, 0},
    {"side_lt",    "Left Top",   "Ctrl+V",            "None",                  0.04, 0.52, 3},
    {"side_lb",    "Left Bottom","Ctrl+C",            "None",                  0.04, 0.68, 3},
    {"side_rt",    "Right Top",  "Ctrl+Y",            "shortcut:next_slice",   0.96, 0.52, 1},
    {"side_rb",    "Right Bot",  "Ctrl+Z",            "None",                  0.96, 0.68, 1},
    {"dial_press", "Dial Press", "KEY_MUTE",          "shortcut:mute_toggle",  0.50, 0.73, 2},
};
constexpr int kPillCount = sizeof(kPillSpecs) / sizeof(kPillSpecs[0]);

QString pillComboStyle()
{
    return QStringLiteral(
        "QComboBox {"
        " background: #1a2a3a;"
        " color: #00b4d8;"
        " border: 1px solid #304050;"
        " border-radius: 11px;"
        " padding: 2px 10px 2px 10px;"
        " font-size: 11px;"
        "}"
        "QComboBox:hover { border-color: #00b4d8; }"
        "QComboBox::drop-down {"
        " border: none;"
        " width: 0;"
        "}"
        "QComboBox::down-arrow {"
        " image: none;"
        " width: 0;"
        " height: 0;"
        "}"
        "QComboBox QAbstractItemView {"
        " background: #1a2a3a;"
        " color: #c8d8e8;"
        " selection-background-color: #0070c0;"
        " border: 1px solid #304050;"
        " font-size: 11px;"
        "}");
}

} // namespace

UlanziDialMapperDialog::UlanziDialMapperDialog(EvdevEncoderManager* manager,
                                               ShortcutManager*     shortcuts,
                                               MidiControlManager*  midi,
                                               QWidget*             parent)
    : PersistentDialog(tr("Ulanzi Dial — Control Mapping"),
                       QStringLiteral("UlanziDialMapperGeom"),
                       parent)
    , m_manager(manager)
    , m_shortcuts(shortcuts)
    , m_midi(midi)
{
    setFixedSize(640, 640);

    auto* outer = new QVBoxLayout(bodyWidget());
    outer->setContentsMargins(0, 0, 0, 8);
    outer->setSpacing(0);

    m_canvas = new UlanziDialCanvas(this);
    outer->addWidget(m_canvas, 1, Qt::AlignCenter);

    auto* bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(12, 0, 12, 0);
    bottomRow->setSpacing(10);

    m_statusLabel = new QLabel(tr("Disconnected"));
    m_statusLabel->setStyleSheet("QLabel { color: #8ea8c0; }");
    bottomRow->addWidget(m_statusLabel);

    bottomRow->addStretch(1);

    m_lastEventLabel = new QLabel(tr("Last event: —"));
    m_lastEventLabel->setStyleSheet("QLabel { color: #8ea8c0; }");
    bottomRow->addWidget(m_lastEventLabel);

    bottomRow->addStretch(1);

    m_resetBtn = new QPushButton(tr("Reset to Defaults"));
    connect(m_resetBtn, &QPushButton::clicked, this, [this] {
        // Reset each combo to its default action.  The signature ↔ pill
        // binding is hardcoded so there's nothing to reset there.
        for (int i = 0; i < m_pills.size(); ++i) {
            if (!m_pills[i].combo) continue;
            const int idx = m_pills[i].combo->findData(m_pills[i].defaultAction);
            if (idx >= 0) m_pills[i].combo->setCurrentIndex(idx);
        }
    });
    bottomRow->addWidget(m_resetBtn);

    m_closeBtn = new QPushButton(tr("Close"));
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottomRow->addWidget(m_closeBtn);

    outer->addLayout(bottomRow);

    buildPills();
    loadActions();

    if (m_manager) {
        connect(m_manager, &EvdevEncoderManager::tuneSteps,
                this, &UlanziDialMapperDialog::onTuneSteps);
        connect(m_manager, &EvdevEncoderManager::buttonEvent,
                this, &UlanziDialMapperDialog::onButtonEvent);
        connect(m_manager, &EvdevEncoderManager::connectionChanged,
                this, &UlanziDialMapperDialog::onConnectionChanged);
        onConnectionChanged(m_manager->isConnected(), m_manager->deviceName());
    } else {
        m_statusLabel->setText(tr("Manager unavailable (Linux build only)"));
    }
}

QPoint UlanziDialMapperDialog::normalizedAnchor(double nx, double ny)
{
    return QPoint(static_cast<int>(nx * 10000), static_cast<int>(ny * 10000));
}

QRect UlanziDialMapperDialog::dialBodyRect() const
{
    // Fixed geometry inside the fixed-size canvas (620 × 555).  Body
    // honours the trimmed image aspect (512:562 ≈ 0.911) at 320×352,
    // centred both horizontally and vertically in the canvas.
    const int bw = 320;
    const int bh = 352;
    const int bx = (620 - bw) / 2;       // 150
    const int by = (555 - bh) / 2;       // 101
    return QRect(bx, by, bw, bh);
}

QPoint UlanziDialMapperDialog::anchorScreenPos(const QPoint& norm) const
{
    const QRect r = dialBodyRect();
    return QPoint(r.left() + (norm.x() * r.width()) / 10000,
                  r.top()  + (norm.y() * r.height()) / 10000);
}

void UlanziDialMapperDialog::buildPills()
{
    m_pills.clear();
    for (int i = 0; i < kPillCount; ++i) {
        Pill p;
        p.id            = QString::fromLatin1(kPillSpecs[i].id);
        p.defaultLabel  = QString::fromLatin1(kPillSpecs[i].label);
        p.signature     = QString::fromLatin1(kPillSpecs[i].signature);
        p.defaultAction = QString::fromLatin1(kPillSpecs[i].defaultAction);
        p.anchor        = normalizedAnchor(kPillSpecs[i].nx, kPillSpecs[i].ny);
        // Pills with hardcoded dialog-coord positions (all three top
        // pills + dial press) are parented to the dialog so move() is
        // in dialog coords.  The side_* pills stay as canvas children.
        const QString pillId = QString::fromLatin1(kPillSpecs[i].id);
        const bool dialogParented =
            (pillId == QLatin1String("top_left")   ||
             pillId == QLatin1String("top_middle") ||
             pillId == QLatin1String("top_right")  ||
             pillId == QLatin1String("dial_press"));
        p.combo = new QComboBox(dialogParented ? static_cast<QWidget*>(this)
                                               : static_cast<QWidget*>(m_canvas));
        p.combo->setStyleSheet(pillComboStyle());
        p.combo->setFixedWidth(120);  // uniform; longer labels get ellipsized
        if (dialogParented) p.combo->raise();

        // (unassigned) — always first.
        p.combo->addItem(tr("(unassigned)"), QStringLiteral("None"));

        // All ShortcutManager actions.  Label format: "[Category] Name"
        // so the dropdown reads as a grouped list even without
        // separators.  Sort happens implicitly by registration order;
        // categories are already grouped at registration time.
        if (m_shortcuts) {
            for (const auto& a : m_shortcuts->actions()) {
                p.combo->addItem(QStringLiteral("[%1] %2").arg(a.category, a.displayName),
                                 QStringLiteral("shortcut:%1").arg(a.id));
            }
        }

        // MIDI Toggle/Trigger params — only the discrete-event types
        // make sense on a button.  Continuous params would need a
        // rotary, which is intentionally not bound here.
#ifdef HAVE_MIDI
        if (m_midi) {
            for (const auto& mp : m_midi->params()) {
                if (mp.type != MidiParamType::Toggle &&
                    mp.type != MidiParamType::Trigger) continue;
                p.combo->addItem(QStringLiteral("[MIDI %1] %2").arg(mp.category, mp.displayName),
                                 QStringLiteral("midi:%1").arg(mp.id));
            }
        }
#endif
        const int idx = i;
        connect(p.combo, &QComboBox::currentIndexChanged,
                this, [this, idx](int) { saveAction(idx); });
        m_pills.append(p);
    }
    layoutPills();
}

void UlanziDialMapperDialog::layoutPills()
{
    if (m_pills.isEmpty()) return;
    const QRect body = dialBodyRect();

    // No connector lines: pills sit "almost touching" the body edge at
    // positions derived directly from pill_id, not anchor coords.  Top
    // pills sit just above the dial, bottom pill just below, side pills
    // hug the left/right edges at the upper- and lower-tab heights.
    auto place = [&](Pill& p) {
        const QSize sz = p.combo->sizeHint();
        const int touchGap = 4;       // tiny visible breathing room
        QPoint center;
        if (p.id == QLatin1String("top_left")) {
            // Dialog-parented — hardcoded dialog coords.
            p.combo->resize(120, p.combo->sizeHint().height());
            p.combo->move(120, 100);
            p.combo->raise();
            p.pillCenter = QPoint(120 + 60, 100 + p.combo->height() / 2);
            return;
        } else if (p.id == QLatin1String("top_middle")) {
            // Pill is a dialog child — hardcode dialog coords.
            // Window 640 wide, combo 120 wide → x = 260.
            // Hardcoded y above the dial body within the dialog.
            p.combo->resize(120, p.combo->sizeHint().height());
            p.combo->move(260, 100);
            p.combo->raise();
            p.pillCenter = QPoint(320, 100 + p.combo->height() / 2);
            return;
        } else if (p.id == QLatin1String("top_right")) {
            // Dialog-parented — hardcoded dialog coords.
            p.combo->resize(120, p.combo->sizeHint().height());
            p.combo->move(400, 100);
            p.combo->raise();
            p.pillCenter = QPoint(400 + 60, 100 + p.combo->height() / 2);
            return;
        } else if (p.id == QLatin1String("side_lt")) {
            center = QPoint(body.left()  - sz.width() / 2 - touchGap + 138,
                            body.top()   + body.height() * 52 / 100 - 45);
        } else if (p.id == QLatin1String("side_lb")) {
            center = QPoint(body.left()  - sz.width() / 2 - touchGap + 138,
                            body.top()   + body.height() * 68 / 100 - 15);
        } else if (p.id == QLatin1String("side_rt")) {
            center = QPoint(body.right() + sz.width() / 2 + touchGap,
                            body.top()   + body.height() * 52 / 100 - 45);
        } else if (p.id == QLatin1String("side_rb")) {
            center = QPoint(body.right() + sz.width() / 2 + touchGap,
                            body.top()   + body.height() * 68 / 100 - 15);
        } else if (p.id == QLatin1String("dial_press")) {
            // Same treatment as top_middle.  Hardcoded x = 640/2 − 120/2 = 260.
            p.combo->resize(120, p.combo->sizeHint().height());
            p.combo->move(260, 525);
            p.combo->raise();
            p.pillCenter = QPoint(320, 525 + p.combo->height() / 2);
            return;
        }
        p.pillCenter = center;
        p.combo->resize(sz);
        p.combo->move(center.x() - sz.width() / 2,
                      center.y() - sz.height() / 2);
    };

    // Bound by m_pills.size() — buildPills() calls refreshPillLabel inside
    // its loop, which re-enters layoutPills before every pill exists.
    for (int i = 0; i < m_pills.size(); ++i) {
        place(m_pills[i]);
    }
}

void UlanziDialCanvas::resizeEvent(QResizeEvent*)
{
    m_owner->layoutPills();
    update();
}

void UlanziDialCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Background — match the surrounding dialog.
    p.fillRect(rect(), QColor("#0f0f1a"));

    const QRect body = m_owner->dialBodyRect();

    const QPixmap& dialImage = ulanziDialPixmap();
    if (!dialImage.isNull()) {
        // body rect now honours image aspect, so a straight stretch is
        // dimensionally exact.  Smooth transform for clean downscale.
        p.drawPixmap(body, dialImage.scaled(body.size(),
                                            Qt::IgnoreAspectRatio,
                                            Qt::SmoothTransformation));
    } else {
        // Fallback: painted stylized representation matching the product
        // shape so layout still looks correct without the asset.
        QPainterPath bodyPath;
        bodyPath.addRoundedRect(body, 22, 22);
        p.fillPath(bodyPath, QColor("#1a2a3a"));
        p.setPen(QPen(QColor("#304050"), 1));
        p.drawPath(bodyPath);

        const int btnW = 56, btnH = 28;
        const int btnY = body.top() + 14;
        const int gapX = (body.width() - btnW * 3) / 4;
        p.setPen(QPen(QColor("#3a4a5a"), 1));
        p.setBrush(QColor("#0f0f1a"));
        for (int i = 0; i < 3; ++i) {
            const int btnX = body.left() + gapX + i * (btnW + gapX);
            p.drawRoundedRect(QRect(btnX, btnY, btnW, btnH), 5, 5);
        }

        const int tabW = 10, tabH = 24;
        p.setBrush(QColor("#1a2a3a"));
        for (double yFrac : { 0.45, 0.65 }) {
            const int ty = body.top() + int(body.height() * yFrac) - tabH / 2;
            p.drawRoundedRect(QRect(body.left() - tabW + 2, ty, tabW, tabH), 3, 3);
            p.drawRoundedRect(QRect(body.right() - 2, ty, tabW, tabH), 3, 3);
        }

        const int knobR = body.width() / 4;
        const QPoint knobCenter(body.center().x(), body.top() + body.height() / 2 + 10);
        p.setBrush(QColor("#0f0f1a"));
        p.setPen(QPen(QColor("#506070"), 2));
        p.drawEllipse(knobCenter, knobR, knobR);
        p.setBrush(QColor("#1a2a3a"));
        p.setPen(QPen(QColor("#3a4a5a"), 1));
        p.drawEllipse(knobCenter, 12, 12);
    }

    // No connector lines / anchor dots — pills sit physically next to
    // the dial body so the spatial mapping is conveyed by adjacency.
}

QString UlanziDialMapperDialog::actionSettingsKey(const QString& pillId)
{
    return QStringLiteral("UlanziDial/action/%1").arg(pillId);
}

QString UlanziDialMapperDialog::defaultActionForPill(const QString& pillId)
{
    for (const auto& spec : kPillSpecs) {
        if (pillId == QLatin1String(spec.id))
            return QString::fromLatin1(spec.defaultAction);
    }
    return QStringLiteral("None");
}

QString UlanziDialMapperDialog::pillForSignature(const QString& signature)
{
    if (signature.isEmpty()) return {};
    for (const auto& spec : kPillSpecs) {
        if (signature == QLatin1String(spec.signature))
            return QString::fromLatin1(spec.id);
    }
    return {};
}

void UlanziDialMapperDialog::loadActions()
{
    auto& s = AppSettings::instance();
    for (int i = 0; i < m_pills.size(); ++i) {
        if (!m_pills[i].combo) continue;
        const QString actionId =
            s.value(actionSettingsKey(m_pills[i].id),
                    m_pills[i].defaultAction).toString();
        const int idx = m_pills[i].combo->findData(actionId);
        if (idx >= 0) m_pills[i].combo->setCurrentIndex(idx);
    }
}

void UlanziDialMapperDialog::saveAction(int pillIndex)
{
    if (pillIndex < 0 || pillIndex >= m_pills.size()) return;
    const Pill& p = m_pills[pillIndex];
    if (!p.combo) return;
    const QString actionId = p.combo->currentData().toString();
    AppSettings::instance().setValue(actionSettingsKey(p.id), actionId);
}

void UlanziDialMapperDialog::refreshPillLabel(int /*pillIndex*/)
{
    // No-op for now: QComboBox handles its own display text.  Kept so
    // future expansion (e.g. showing the signature alongside the combo)
    // has a single entry point.
}

void UlanziDialMapperDialog::onTuneSteps(int steps)
{
    if (m_lastEventLabel)
        m_lastEventLabel->setText(tr("Last event: rotary %1")
                                      .arg(steps > 0 ? "CW" : "CCW"));
}

void UlanziDialMapperDialog::onButtonEvent(const QString& signature, int action)
{
    if (!m_lastEventLabel) return;
    const QString pillId = pillForSignature(signature);
    const QString suffix = pillId.isEmpty()
        ? tr(" (unmapped)")
        : QStringLiteral(" → %1").arg(pillId);
    m_lastEventLabel->setText(tr("Last event: %1%2 (%3)")
                                  .arg(signature, suffix,
                                       action == 1 ? "press" : "release"));
}

void UlanziDialMapperDialog::showEvent(QShowEvent* event)
{
    PersistentDialog::showEvent(event);
    // The first layoutPills happens from buildPills (constructor),
    // before Qt has positioned m_canvas inside the dialog.  After the
    // dialog is shown, m_canvas->mapFrom(this, ...) returns the right
    // values, so re-run the layout to put the dialog-centred pills
    // (top_middle, dial_press) at their proper x.
    layoutPills();
    if (m_canvas) m_canvas->update();
}

void UlanziDialMapperDialog::onConnectionChanged(bool connected, const QString& name)
{
    if (!m_statusLabel) return;
    QString display = name;
    if (display.endsWith(QStringLiteral(" Keyboard"), Qt::CaseInsensitive))
        display.chop(QStringLiteral(" Keyboard").size());
    m_statusLabel->setText(connected
        ? tr("Connected — %1").arg(display)
        : tr("Disconnected"));
    m_statusLabel->setStyleSheet(connected
        ? "QLabel { color: #4dd87a; }"
        : "QLabel { color: #cc3333; }");
}

} // namespace AetherSDR

#endif // Q_OS_LINUX
