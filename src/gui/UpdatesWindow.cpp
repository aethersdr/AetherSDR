#include "gui/UpdatesWindow.h"

#include "core/ThemeManager.h"
#include "core/UpdateChecker.h"
#include "core/UpdateController.h"
#include "gui/CanonIndicators.h"
#include "gui/FirmwareUpdateCard.h"
#include "gui/FlowLayout.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// The order chapter dots take the category colors in.
const char* const kCategoryTokens[] = {
    "color.canon.category.cyan",  "color.canon.category.aqua", "color.canon.category.blue",
    "color.canon.category.amber", "color.canon.category.violet", "color.canon.category.rose",
    "color.canon.category.teal",
};
constexpr int kCategoryCount = sizeof(kCategoryTokens) / sizeof(kCategoryTokens[0]);

// The big version numerals: the brand gradient for a new release, canon ink
// otherwise. Painted, so the gradient can fill the glyphs; the accessible
// name carries the same text.
class VersionNumerals final : public QWidget {
public:
    explicit VersionNumerals(QWidget* parent) : QWidget(parent)
    {
        // Its own font: a window-wide stylesheet font would otherwise win.
        m_font = font();
        m_font.setPixelSize(60);
        m_font.setWeight(QFont::ExtraBold);
        m_font.setLetterSpacing(QFont::PercentageSpacing, 96);
        // The "v" is set a little smaller, on the numerals' baseline.
        m_prefixFont = m_font;
        m_prefixFont.setPixelSize(48);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
    }
    // The label the numerals sit beside: their baseline is drawn level with
    // its baseline when both are bottom-aligned in a row.
    void setBaselinePartner(const QLabel* partner) { m_partner = partner; }
    void setVersion(const QString& version, bool gradient)
    {
        m_text = version;
        m_gradient = gradient;
        setAccessibleName(version.isEmpty() ? QString() : QStringLiteral("Version %1").arg(version));
        updateGeometry();
        update();
    }
    QSize sizeHint() const override
    {
        const QFontMetrics fm(m_font);
        return {prefixAdvance() + fm.horizontalAdvance(m_text) + 4, fm.ascent() + belowBaseline()};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        const QFontMetrics fm(m_font);
        // Winding, not the default odd-even: tight letter spacing overlaps
        // glyphs, and odd-even would cut a hole where they meet.
        path.setFillRule(Qt::WindingFill);
        if (!m_text.isEmpty()) {
            path.addText(0, fm.ascent(), m_prefixFont, kPrefix);
            path.addText(prefixAdvance(), fm.ascent(), m_font, m_text);
        }
        auto& tm = ThemeManager::instance();
        p.fillPath(path, m_gradient
                             ? tm.brush(QStringLiteral("color.brand.gradient"), path.boundingRect().toRect())
                             : QBrush(tm.color(this, QStringLiteral("color.canon.ink"))));
    }

private:
    static inline const QString kPrefix = QStringLiteral("v");

    int prefixAdvance() const
    {
        return m_text.isEmpty() ? 0 : QFontMetrics(m_prefixFont).horizontalAdvance(kPrefix);
    }

    // How far the partner's baseline sits above its bottom edge.
    int belowBaseline() const
    {
        if (!m_partner) {
            return QFontMetrics(m_font).descent();
        }
        return QFontMetrics(m_partner->font()).descent() + m_partner->contentsMargins().bottom()
               + m_partner->margin();
    }

    QPointer<const QLabel> m_partner;
    QString m_text;
    QFont m_font;
    QFont m_prefixFont;
    bool m_gradient{false};
};

QLabel* plainLabel(const QString& role, QWidget* parent = nullptr)
{
    auto* l = new QLabel(parent);
    l->setTextFormat(Qt::PlainText);   // release text is network text
    l->setProperty("role", role);
    return l;
}

QString sheet()
{
    return QStringLiteral(
               "QLabel { background: transparent; color: {{color.canon.inkSoft}}; }"
               // Plain containers stay see-through over the canon ground.
               "QWidget#updatesHeader, QWidget#updatesVersion, QWidget#updatesChapters,"
               " QWidget#updatesContent, QScrollArea#updatesScroll,"
               " QScrollArea#updatesScroll > QWidget > QWidget { background: transparent; border: none; }"
               "QLabel[role=\"eyebrow\"] { color: {{color.canon.cyan}}; font-size: 11px; font-weight: 700;"
               " letter-spacing: 1.5px; }"
               "QLabel[role=\"eyebrow\"][tone=\"current\"] { color: {{color.canon.aqua}}; }"
               "QLabel[role=\"eyebrow\"][tone=\"muted\"] { color: {{color.canon.muted}}; }"
               "QLabel[role=\"product\"] { color: {{color.canon.ink}}; font-size: 26px; font-weight: 700; }"
               "QLabel[role=\"tagline\"] { color: {{color.canon.inkSoft}}; font-size: 15px; }"
               "QLabel[role=\"section\"] { color: {{color.canon.muted}}; font-size: 11px; font-weight: 700;"
               " letter-spacing: 1.5px; }"
               "QLabel[role=\"note\"], QLabel[role=\"footnote\"] { color: {{color.canon.muted}}; font-size: 12px; }"
               "QLabel[role=\"chip\"] { color: {{color.canon.inkSoft}}; font-size: 12px; }"
               "QFrame#updatesChip { background: {{color.canon.raised}}; border: 1px solid {{color.canon.lineHi}};"
               " border-radius: 12px; }"
               "QPushButton#updatesGet { background: {{color.brand.gradient}}; color: {{color.canon.onAccent}};"
               " font-size: 15px; font-weight: 700; border: 1px solid transparent; border-radius: 12px;"
               " padding: 11px 22px; }"
               "QPushButton#updatesGet:hover { border-color: {{color.canon.aqua}}; }"
               "QPushButton#updatesGet:focus { border: 2px solid {{color.canon.aqua}}; padding: 10px 21px; }"
               "QPushButton#updatesSecondary { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
               " border: 1px solid {{color.canon.lineHi}}; border-radius: 10px; padding: 9px 14px;"
               " font-weight: 600; }"
               "QPushButton#updatesSecondary:hover { background: {{color.canon.nested}};"
               " color: {{color.canon.aqua}}; }"
               "QPushButton#updatesSecondary:focus { border-color: {{color.canon.aqua}}; }"
               "QPushButton#updatesGhost { background: transparent; color: {{color.canon.muted}}; border: none;"
               " padding: 9px 6px; font-weight: 600; }"
               "QPushButton#updatesGhost:hover { color: {{color.canon.inkSoft}}; }"
               "QPushButton#updatesGhost:focus { color: {{color.canon.ink}}; }"
               "QPushButton:disabled { color: {{color.canon.muted}}; }"
               // An ID rule outranks the bare :disabled one, so each styled
               // button states its own dimmed look.
               "QPushButton#updatesSecondary:disabled { color: {{color.canon.muted}};"
               " background: {{color.canon.nested}}; border-color: {{color.canon.line}}; }"
               "QPushButton#updatesGet:disabled { color: {{color.canon.muted}};"
               " background: {{color.canon.nested}}; border-color: {{color.canon.line}}; }"
               "QPushButton#updatesGhost:disabled { color: {{color.canon.muted}}; }"
               "QPushButton#firmwareRelease:disabled { color: {{color.canon.muted}}; }"
               "QCheckBox { color: {{color.canon.inkSoft}}; background: transparent; }"
               "QFrame#updatesFirmwareCard { background: {{color.canon.raised}}; border: 1px solid {{color.canon.line}};"
               " border-radius: 14px; }"
               "QLabel[role=\"radioName\"] { color: {{color.canon.ink}}; font-size: 15px; font-weight: 650; }"
               "QLabel[role=\"radioDetail\"], QLabel[role=\"lane\"] { color: {{color.canon.muted}}; font-size: 12px; }"
               "QLabel[role=\"pill\"] { color: {{color.canon.muted}}; border: 1px solid {{color.canon.muted}};"
               " border-radius: 10px; padding: 2px 10px; font-size: 12px; font-weight: 650; }"
               "QLabel[role=\"pill\"][tone=\"warning\"] { color: {{color.accent.warning}}; border-color: {{color.accent.warning}}; }"
               "QLabel[role=\"pill\"][tone=\"current\"] { color: {{color.canon.aqua}}; border-color: {{color.canon.aqua}}; }"
               "QFrame#updatesFirmwareCard QLabel[role=\"eyebrow\"] { color: {{color.accent.warning}}; }"
               "QPushButton#firmwareRelease { background: {{color.canon.nested}}; color: {{color.canon.inkSoft}};"
               " border: 1px solid {{color.canon.line}}; border-radius: 10px; padding: 4px 10px;"
               " font-family: \"SF Mono\", Menlo, Consolas, monospace; font-size: 12px; }"
               "QPushButton#firmwareRelease[kind=\"installed\"] { border: 2px solid {{color.canon.inkSoft}}; }"
               "QPushButton#firmwareRelease[kind=\"newest\"] { color: {{color.accent.warning}};"
               " border-color: {{color.accent.warning}}; }"
               "QPushButton#firmwareRelease:checked { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
               " border-color: {{color.canon.cyan}}; }"
               "QPushButton#firmwareRelease:focus { border-color: {{color.canon.aqua}}; }"
               "QLabel[role=\"step\"] { color: {{color.canon.muted}}; background: {{color.canon.nested}};"
               " border: 1px solid {{color.canon.line}}; border-radius: 10px; padding: 8px; font-size: 12px; }"
               "QLabel[role=\"step\"][state=\"done\"] { color: {{color.canon.category.teal}}; }"
               "QLabel[role=\"step\"][state=\"now\"] { color: {{color.canon.ink}}; border-color: {{color.canon.cyan}}; }"
               "QLabel[role=\"status\"] { color: {{color.canon.inkSoft}}; font-size: 12px; }"
               "QLabel[role=\"status\"][tone=\"warning\"] { color: {{color.accent.warning}}; }"
               "QLabel[role=\"status\"][tone=\"danger\"] { color: {{color.accent.danger}}; }"
               "QLabel[role=\"status\"][tone=\"current\"] { color: {{color.canon.aqua}}; }"
               "QLabel[role=\"caution\"] { color: {{color.accent.warning}}; font-size: 12px; }"
               "QProgressBar#firmwareProgress, QProgressBar#updatesProgress { background: {{color.canon.nested}};"
               " border: 1px solid {{color.canon.line}}; border-radius: 4px; }"
               "QProgressBar#firmwareProgress::chunk, QProgressBar#updatesProgress::chunk {"
               " background: {{color.brand.gradient}}; border-radius: 3px; }"
               "QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }"
               "QScrollBar::handle:vertical { background: {{color.canon.lineHi}}; border-radius: 3px;"
               " min-height: 24px; }"
               "QScrollBar::handle:vertical:hover { background: {{color.canon.cyan}}; }"
               "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
               "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }")
        + canonIndicatorRules();
}

} // namespace

UpdatesWindow::UpdatesWindow(UpdateChecker* checker, RadioModel* radio, QWidget* parent)
    : CanonWindow(tr("Updates"), parent, Kind::Workspace)
    , m_checker(checker)
{
    setObjectName(QStringLiteral("updatesWindow"));
    setGeometryKey(QStringLiteral("UpdatesWindowGeometry"));
    setLaunchSize(QSize(760, 780));   // room for YOUR RADIO under the release
    applyCanonSheet(bodyWidget(), sheet);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(0, 0, 0, 22);
    root->setSpacing(0);

    // The header: what the window is about and the release's name.
    m_header = new QWidget(bodyWidget());
    m_header->setObjectName(QStringLiteral("updatesHeader"));
    auto* over = new QVBoxLayout(m_header);
    over->setContentsMargins(28, 22, 28, 18);
    over->setSpacing(6);
    m_eyebrow = plainLabel(QStringLiteral("eyebrow"));
    m_eyebrow->setObjectName(QStringLiteral("updatesEyebrow"));
    over->addWidget(m_eyebrow);
    auto* nameRow = new QHBoxLayout;
    nameRow->setSpacing(14);
    auto* product = plainLabel(QStringLiteral("product"));
    product->setText(QStringLiteral("AetherSDR"));
    nameRow->addWidget(product, 0, Qt::AlignBottom);
    auto* numerals = new VersionNumerals(m_header);
    numerals->setObjectName(QStringLiteral("updatesVersion"));
    numerals->setBaselinePartner(product);
    m_version = numerals;
    nameRow->addWidget(numerals, 0, Qt::AlignBottom);
    nameRow->addStretch(1);
    over->addLayout(nameRow);
    m_tagline = plainLabel(QStringLiteral("tagline"));
    m_tagline->setObjectName(QStringLiteral("updatesTagline"));
    m_tagline->setWordWrap(true);
    over->addWidget(m_tagline);
    root->addWidget(m_header);

    // Everything under the header scrolls, so a long release never makes the
    // window taller than the screen.
    auto* scroll = new QScrollArea;
    m_scroll = scroll;
    scroll->setObjectName(QStringLiteral("updatesScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* contentWidget = new QWidget;
    contentWidget->setObjectName(QStringLiteral("updatesContent"));
    auto* content = new QVBoxLayout(contentWidget);
    content->setContentsMargins(28, 16, 28, 4);
    content->setSpacing(12);
    scroll->setWidget(contentWidget);
    root->addWidget(scroll, 1);

    m_note = plainLabel(QStringLiteral("note"));
    m_note->setObjectName(QStringLiteral("updatesNote"));
    m_note->setWordWrap(true);
    content->addWidget(m_note);

    m_chaptersTitle = plainLabel(QStringLiteral("section"));
    m_chaptersTitle->setObjectName(QStringLiteral("updatesChaptersTitle"));
    content->addWidget(m_chaptersTitle);
    m_chips = new QWidget;
    m_chips->setObjectName(QStringLiteral("updatesChapters"));
    QSizePolicy chipsPolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    chipsPolicy.setHeightForWidth(true);
    m_chips->setSizePolicy(chipsPolicy);
    m_chipsLayout = new FlowLayout(0, 6, 6);
    m_chips->setLayout(m_chipsLayout);
    content->addWidget(m_chips);


    auto* actions = new QHBoxLayout;
    actions->setSpacing(10);
    m_get = new QPushButton;
    m_get->setObjectName(QStringLiteral("updatesGet"));
    m_get->setCursor(Qt::PointingHandCursor);
    actions->addWidget(m_get);
    m_download = new QPushButton(tr("Download installer"));
    m_download->setObjectName(QStringLiteral("updatesGhost"));
    m_download->setCursor(Qt::PointingHandCursor);
    m_download->setAccessibleName(tr("Download installer"));
    actions->addWidget(m_download);
    m_notes = new QPushButton(tr("Full release notes"));
    m_notes->setObjectName(QStringLiteral("updatesSecondary"));
    m_notes->setCursor(Qt::PointingHandCursor);
    m_notes->setAccessibleDescription(tr("Opens the release page on GitHub in your browser."));
    actions->addWidget(m_notes);
    m_checkNow = new QPushButton(tr("Check Now"));
    m_checkNow->setObjectName(QStringLiteral("updatesSecondary"));
    m_checkNow->setCursor(Qt::PointingHandCursor);
    actions->addWidget(m_checkNow);
    m_skip = new QPushButton(tr("Skip this version"));
    m_skip->setObjectName(QStringLiteral("updatesGhost"));
    m_skip->setCursor(Qt::PointingHandCursor);
    m_skip->setAccessibleDescription(
        tr("Stop showing UPDATE in the title bar for this release. The next release shows it again."));
    actions->addWidget(m_skip);
    actions->addStretch(1);
    content->addLayout(actions);

    // Install update in progress: what it is doing, how far, and Cancel.
    m_progressRow = new QWidget;
    m_progressRow->setObjectName(QStringLiteral("updatesInstallProgress"));
    auto* progressRow = new QVBoxLayout(m_progressRow);
    progressRow->setContentsMargins(0, 0, 0, 0);
    progressRow->setSpacing(6);
    auto* progressLine = new QHBoxLayout;
    m_progressLabel = plainLabel(QStringLiteral("status"));
    m_progressLabel->setObjectName(QStringLiteral("updatesInstallStatus"));
    m_progressLabel->setAccessibleName(tr("Update install status"));
    progressLine->addWidget(m_progressLabel, 1);
    m_cancel = new QPushButton(tr("Cancel"));
    m_cancel->setObjectName(QStringLiteral("updatesSecondary"));
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_cancel->setAccessibleName(tr("Cancel the update"));
    m_cancel->setAccessibleDescription(tr("Stops downloading the update. Nothing is changed."));
    progressLine->addWidget(m_cancel);
    progressRow->addLayout(progressLine);
    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("updatesProgress"));
    m_progress->setAccessibleName(tr("Update download progress"));
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(8);
    progressRow->addWidget(m_progress);
    m_progressRow->hide();
    content->addWidget(m_progressRow);

    auto* footer = new QHBoxLayout;
    m_footnote = plainLabel(QStringLiteral("footnote"));
    m_footnote->setObjectName(QStringLiteral("updatesFootnote"));
    m_footnote->setWordWrap(true);
    footer->addWidget(m_footnote, 1);
    m_autoCheck = new QCheckBox(tr("Check daily"));
    m_autoCheck->setObjectName(QStringLiteral("updatesCheckDaily"));
    m_autoCheck->setAccessibleDescription(
        tr("Check for a newer release at startup, at most once a day, and show UPDATE in the title bar."));
    footer->addWidget(m_autoCheck, 0, Qt::AlignRight);
    content->addLayout(footer);

    if (radio) {
        content->addSpacing(10);
        m_firmware = new FirmwareUpdateCard(radio, contentWidget);
        content->addWidget(m_firmware);
        connect(m_firmware, &FirmwareUpdateCard::busyChanged, this, [this](bool busy) {
            m_firmwareBusy = busy;
            refresh();
        });
    }
    content->addStretch(1);

    const QString store = UpdateChecker::storeName();
    m_autoCheck->setChecked(store.isEmpty() && UpdateChecker::checkOnStartup());
    m_autoCheck->setEnabled(store.isEmpty());
    if (!store.isEmpty()) {
        m_autoCheck->setToolTip(tr("Updates for this copy come from %1.").arg(store));
        m_autoCheck->setAccessibleDescription(m_autoCheck->toolTip());
    }

    connect(m_get, &QPushButton::clicked, this, &UpdatesWindow::primaryClicked);
    connect(m_download, &QPushButton::clicked, this, &UpdatesWindow::openDownload);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_controller) {
            m_controller->cancel();
        }
    });
    connect(m_notes, &QPushButton::clicked, this, [this] {
        const QString version = m_checker ? m_checker->latestRelease().version : QString();
        QDesktopServices::openUrl(UpdateChecker::releasePageUrl(version));
    });
    connect(m_checkNow, &QPushButton::clicked, this, &UpdatesWindow::checkNow);
    connect(m_skip, &QPushButton::clicked, this, [this] {
        if (!m_checker) {
            return;
        }
        const QString version = m_checker->latestRelease().version;
        UpdateChecker::skipVersion(version);
        // Skipping a release that is staged, or being staged, discards it.
        if (m_controller) {
            m_controller->discard(version);
        }
        emit badgeChanged(QString());
        refresh();
    });
    connect(m_autoCheck, &QCheckBox::toggled, this, &UpdatesWindow::automaticCheckToggled);

    if (m_checker) {
        // A check started anywhere (the daily check, Help) shows at once.
        connect(m_checker, &UpdateChecker::checkStarted, this, &UpdatesWindow::refresh);
        connect(m_checker, &UpdateChecker::releaseInfoChanged, this, [this] {
            m_failed = false;
            refresh();
        });
        connect(m_checker, &UpdateChecker::checkFailed, this, [this] {
            m_failed = true;
            refresh();
        });
        // Nothing known, or only a version saved by an earlier answer: fetch
        // the release's details.
        if ((!m_checker->latestRelease().isValid() || m_checker->latestRelease().fromSettings)
            && !m_checker->isChecking()) {
            m_checker->checkNow(UpdateChecker::Trigger::Manual);
        }
    }
    refresh();
}

void UpdatesWindow::checkNow()
{
    if (!m_checker) {
        return;
    }
    m_failed = false;
    m_checker->checkNow(UpdateChecker::Trigger::Manual);
    refresh();
}

void UpdatesWindow::setAutomaticCheck(bool on)
{
    const QSignalBlocker block(m_autoCheck);
    m_autoCheck->setChecked(on && UpdateChecker::storeName().isEmpty());
}

void UpdatesWindow::setController(UpdateController* controller)
{
    if (m_controller) {
        disconnect(m_controller, nullptr, this, nullptr);
    }
    m_controller = controller;
    // Asked once: it probes the install folder.
    m_canInPlace = controller && controller->canInstallInPlace();
    if (controller) {
        connect(controller, &UpdateController::stateChanged, this, &UpdatesWindow::refresh);
        connect(controller, &UpdateController::progress, this,
                [this](const QString& step, qint64 done, qint64 total) {
                    if (total > 0) {
                        const int percent = static_cast<int>(done * 100 / total);
                        m_progress->setRange(0, 100);
                        m_progress->setValue(percent);
                        m_progressLabel->setText(tr("%1… %2%").arg(step).arg(percent));
                    } else {
                        m_progress->setRange(0, 0);   // busy: no size to count against
                        m_progressLabel->setText(tr("%1…").arg(step));
                    }
                    m_progress->setAccessibleDescription(m_progressLabel->text());
                });
        // Each outcome is kept with the release it was about, not whatever
        // the check shows now.
        connect(controller, &UpdateController::browserNeeded, this,
                [this](const QString& reason, const QString& version) {
                    m_browserReason = reason;
                    m_browserVersion = version;
                    refresh();
                });
        connect(controller, &UpdateController::failed, this, [this](const QString& reason, const QString& version) {
            m_installProblem = reason;
            m_problemVersion = version;
            refresh();
        });
    }
    refresh();
}

void UpdatesWindow::showInstallProblem(const QString& problem, const QString& version)
{
    m_installProblem = problem;
    m_problemVersion = version;
    refresh();
}

bool UpdatesWindow::installsInPlace(const QString& version) const
{
    return m_controller && m_canInPlace && !m_controller->isRolledBack(version)
           && !(m_browserVersion == version && !m_browserReason.isEmpty());
}

void UpdatesWindow::primaryClicked()
{
    if (!m_checker) {
        return;
    }
    const QString version = m_checker->latestRelease().version;
    if (!installsInPlace(version)) {
        openDownload();
        return;
    }
    if (m_controller->state() == UpdateController::State::Staged && m_controller->version() == version) {
        emit restartRequested();
        return;
    }
    if (m_controller->state() != UpdateController::State::Staging
        && m_controller->state() != UpdateController::State::Handed) {
        m_installProblem.clear();
        // Installing a skipped release means it is wanted after all.
        if (UpdateChecker::isSkipped(version)) {
            UpdateChecker::unskipVersion(version);
            emit badgeChanged(version);
        }
        m_controller->install(version);
    }
}

UpdatesWindow::View UpdatesWindow::currentView() const
{
    static const ReleaseInfo kNone;
    const ReleaseInfo& release = m_checker ? m_checker->latestRelease() : kNone;
    const auto pending = [this] {
        return (m_checker && m_checker->isChecking()) || !m_failed ? View::Checking : View::Failed;
    };
    if (!release.isValid()) {
        return pending();
    }
    if (UpdateChecker::isNewer(release.version, QCoreApplication::applicationVersion())) {
        return View::Available;   // a saved newer release stays actionable offline
    }
    // A saved version that is not newer says nothing about GitHub now: only a
    // real answer may report "up to date".
    return release.fromSettings ? pending() : View::Current;
}

void UpdatesWindow::refresh()
{
    const View view = currentView();
    const QString running = QCoreApplication::applicationVersion();
    const QString store = UpdateChecker::storeName();
    static const ReleaseInfo kNone;
    const bool known = m_checker && m_checker->latestRelease().isValid();
    const ReleaseInfo& release = known ? m_checker->latestRelease() : kNone;
    auto* numerals = static_cast<VersionNumerals*>(m_version);

    // A version saved by an earlier answer carries no details: build only
    // from a real answer. Keyed on the chapters, not the version, so release
    // notes edited after publication are rebuilt too.
    if (known && !release.fromSettings && m_builtChapters != release.chapters) {
        rebuildRelease();
    }

    QString eyebrow;
    QString tone;
    QString note;
    switch (view) {
    case View::Available: {
        eyebrow = tr("NEW RELEASE");
        if (release.published.isValid()) {
            eyebrow += QStringLiteral(" · ")
                       + QLocale::c().toString(release.published.date(), QStringLiteral("MMM d, yyyy")).toUpper();
        }
        numerals->setVersion(release.version, true);
        m_tagline->setText(release.tagline);
        if (!store.isEmpty()) {
            note = tr("Updates for this copy come from %1.").arg(store);
        } else if (m_failed && release.fromSettings) {
            note = tr("Couldn't reach GitHub for this release's details. Check your connection, then try again.");
        } else if (m_failed) {
            note = tr("Couldn't reach GitHub just now. This is its last answer.");
        } else if (m_controller && m_controller->isRolledBack(release.version)) {
            note = tr("This release didn't start on this computer, so it was rolled back and isn't installed "
                      "here again.");
        } else if (m_browserVersion == release.version && !m_browserReason.isEmpty()) {
            note = tr("The update couldn't be prepared here: %1 Download it instead.").arg(m_browserReason);
        } else if (!m_installProblem.isEmpty() && m_problemVersion == release.version) {
            note = m_installProblem;
        } else if (UpdateChecker::isSkipped(release.version)) {
            note = tr("You skipped this release, so UPDATE is not shown in the title bar.");
        }
        break;
    }
    case View::Current:
        eyebrow = QStringLiteral("✓ ") + tr("UP TO DATE");
        tone = QStringLiteral("current");
        numerals->setVersion(running, false);
        m_tagline->setText(UpdateChecker::isNewer(running, release.version)
                               ? tr("You're running a newer build than the latest release, v%1.")
                                     .arg(release.version)
                               : tr("You're on the newest release."));
        if (m_failed) {
            note = tr("Couldn't reach GitHub just now. This is its last answer.");
        }
        break;
    case View::Checking:
        eyebrow = tr("CHECKING FOR UPDATES…");
        tone = QStringLiteral("muted");
        numerals->setVersion(running, false);
        m_tagline->setText(tr("Asking GitHub for the newest release."));
        break;
    case View::Failed:
        eyebrow = tr("COULDN'T REACH GITHUB");
        tone = QStringLiteral("muted");
        numerals->setVersion(running, false);
        m_tagline->setText(tr("Check your connection, then try again."));
        break;
    }
    m_eyebrow->setText(eyebrow);
    m_eyebrow->setProperty("tone", tone);
    m_eyebrow->style()->unpolish(m_eyebrow);
    m_eyebrow->style()->polish(m_eyebrow);
    m_note->setText(note);
    m_note->setVisible(!note.isEmpty());

    const bool showRelease = known && (view == View::Available || view == View::Current);
    m_chaptersTitle->setVisible(showRelease && !release.chapters.isEmpty());
    m_chips->setVisible(showRelease && !release.chapters.isEmpty());

    const ReleaseAsset asset = UpdateChecker::assetForThisComputer(release);
    const bool canDownload = view == View::Available && store.isEmpty();
    // Install update, in place, when the controller can; the browser
    // otherwise, or once this release fell back to it.
    using State = UpdateController::State;
    const bool inPlace = canDownload && installsInPlace(release.version);
    const State installState = m_controller ? m_controller->state() : State::Idle;
    const bool forThis = m_controller && m_controller->version() == release.version;
    // Whatever release it is staging: a newer latest heard meanwhile waits
    // until it finishes (the controller then discards it).
    const bool staging = inPlace && installState == State::Staging;
    const bool staged = inPlace && forThis && (installState == State::Staged || installState == State::Handed);
    m_get->setVisible(canDownload && !staging);
    m_progressRow->setVisible(staging);
    // The browser route stays beside Install update (Phase 3 ruling).
    m_download->setVisible(inPlace && !staging);
    m_download->setAccessibleDescription(asset.url.isValid()
                                             ? tr("Downloads %1 in your browser instead.").arg(asset.name)
                                             : tr("Opens the release page to choose a download instead."));
    if (staged) {
        m_get->setText(tr("Restart to update"));
        m_get->setAccessibleName(tr("Restart to update to AetherSDR %1").arg(release.version));
        m_get->setAccessibleDescription(
            tr("Quits AetherSDR, installs v%1 and starts it again. Otherwise it installs when you quit.")
                .arg(release.version));
    } else if (inPlace) {
        m_get->setText(tr("Install update"));
        m_get->setAccessibleName(tr("Install AetherSDR %1").arg(release.version));
        m_get->setAccessibleDescription(
            tr("Downloads v%1 and gets it ready. Nothing changes until AetherSDR restarts or quits.")
                .arg(release.version));
    } else {
        m_get->setText(tr("Get AetherSDR %1  →").arg(release.version));
        m_get->setAccessibleName(tr("Get AetherSDR %1").arg(release.version));
        m_get->setAccessibleDescription(asset.url.isValid()
                                            ? tr("Downloads %1 in your browser.").arg(asset.name)
                                            : tr("Opens the release page to choose a download."));
    }
    m_notes->setVisible(known);
    // Offered whenever there is something to retry: always outside a new
    // release, and inside one whose details did not arrive.
    m_checkNow->setVisible(view != View::Available || (m_failed && release.fromSettings));
    const bool checking = m_checker && m_checker->isChecking();
    m_checkNow->setEnabled(!checking && !m_firmwareBusy);
    m_checkNow->setText(checking ? tr("Checking…") : tr("Check Now"));
    m_skip->setVisible(canDownload);
    const bool skipped = UpdateChecker::isSkipped(release.version);
    m_skip->setEnabled(!skipped && !m_firmwareBusy);
    m_skip->setText(skipped ? tr("Skipped") : tr("Skip this version"));
    // One job at a time: nothing else starts while radio firmware is going in.
    const QString waiting = tr("Waits until the radio firmware install finishes.");
    for (QWidget* w : {static_cast<QWidget*>(m_get), static_cast<QWidget*>(m_checkNow),
                       static_cast<QWidget*>(m_autoCheck)}) {
        if (m_firmwareBusy) {
            w->setEnabled(false);
            w->setToolTip(waiting);
            w->setAccessibleDescription(waiting);
        } else if (w->toolTip() == waiting) {
            w->setToolTip(QString());
            if (w != m_get) {   // the primary button's was just set above
                w->setAccessibleDescription(QString());
            }
        }
    }
    if (!m_firmwareBusy) {
        // Handed to the helper: AetherSDR is quitting.
        m_get->setEnabled(!(staged && installState == State::Handed));
        m_autoCheck->setEnabled(UpdateChecker::storeName().isEmpty());
    }

    if (staged) {
        m_footnote->setText(tr("AetherSDR %1 is ready. It installs when you quit, or now with Restart to update.")
                                .arg(release.version));
    } else if (staging) {
        m_footnote->setText(tr("Nothing changes until AetherSDR restarts or quits. Closing this window stops it."));
    } else if (inPlace) {
        m_footnote->setText(tr("Downloads the update built for this computer. It installs when AetherSDR restarts "
                               "or quits."));
    } else if (canDownload) {
        m_footnote->setText(asset.url.isValid()
                                ? tr("Downloads %1, the file built for this computer.").arg(asset.name)
                                : tr("Opens the release page, where you choose the download for this computer."));
    } else if (view == View::Current) {
        // When GitHub last answered, not the last attempt: a failed daily
        // check also starts the daily wait.
        const QDateTime answered = release.answered;
        m_footnote->setText(answered.isValid()
                                ? tr("GitHub answered %1.").arg(QLocale().toString(answered.toLocalTime(),
                                                                                  QLocale::ShortFormat))
                                : QString());
    } else {
        m_footnote->clear();
    }

    m_header->setAccessibleName(tr("Updates"));
    m_header->setAccessibleDescription(view == View::Available
                                           ? tr("Installed v%1; v%2 is available.").arg(running, release.version)
                                           : tr("Installed v%1.").arg(running));
}

void UpdatesWindow::showFirmware()
{
    if (m_firmware && m_scroll) {
        m_scroll->ensureWidgetVisible(m_firmware, 0, 24);
    }
}

// Every close path (the corner button, Escape, ⌘W, reject(), accept()) ends
// here: QDialog's own closeEvent goes through reject().
void UpdatesWindow::done(int result)
{
    if (!m_firmware || m_firmware->confirmStop(this)) {
        // Closing stops an update still being staged (a staged one stays).
        if (m_controller && m_controller->state() == UpdateController::State::Staging) {
            m_controller->cancel();
        }
        CanonWindow::done(result);
    }
}

void UpdatesWindow::rebuildRelease()
{
    const ReleaseInfo& release = m_checker->latestRelease();
    m_builtChapters = release.chapters;

    while (QLayoutItem* item = m_chipsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_chaptersTitle->setText(release.chapters.size() == 1
                                 ? tr("WHAT'S INSIDE · 1 CHAPTER")
                                 : tr("WHAT'S INSIDE · %1 CHAPTERS").arg(release.chapters.size()));
    auto& tm = ThemeManager::instance();
    for (int i = 0; i < release.chapters.size(); ++i) {
        auto* chip = new QFrame(m_chips);
        chip->setObjectName(QStringLiteral("updatesChip"));
        auto* row = new QHBoxLayout(chip);
        row->setContentsMargins(10, 4, 12, 4);
        row->setSpacing(7);
        auto* dot = new QFrame(chip);
        dot->setFixedSize(7, 7);
        tm.applyStyleSheet(dot, QStringLiteral("QFrame { background: {{%1}}; border-radius: 3px; }")
                                    .arg(QString::fromLatin1(kCategoryTokens[i % kCategoryCount])));
        row->addWidget(dot);
        auto* text = plainLabel(QStringLiteral("chip"), chip);
        text->setText(release.chapters[i]);
        row->addWidget(text);
        m_chipsLayout->addWidget(chip);
    }

}

void UpdatesWindow::openDownload()
{
    if (!m_checker) {
        return;
    }
    const ReleaseInfo& release = m_checker->latestRelease();
    const ReleaseAsset asset = UpdateChecker::assetForThisComputer(release);
    // assetForThisComputer() only returns URLs that passed the allowlist.
    QDesktopServices::openUrl(asset.url.isValid() ? asset.url : UpdateChecker::releasePageUrl(release.version));
}

} // namespace AetherSDR
